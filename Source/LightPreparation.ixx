module;

#define NOMINMAX

#include "directx/d3dx12.h"

#include <DirectXMath.h>

#include "directxtk12/ResourceUploadBatch.h"

#include "rtxdi/ReSTIRDIParameters.h"

#include "Shaders/LightPreparation.dxil.h"

export module LightPreparation;

import CommonShaderData;
import ErrorHelpers;
import GPUBuffer;
import RTXDIResources;
import Scene;
import Texture;

using namespace DirectX;
using namespace ErrorHelpers;
using namespace Microsoft::WRL;
using namespace std;

namespace {
	auto IsEmissive(const RenderObject& renderObject) {
		constexpr auto Max = [](const XMFLOAT3& value) { return max({ value.x, value.y, value.z }); };
		return Max(renderObject.Material.EmissiveColor) > 0 || renderObject.Textures.contains(TextureType::EmissiveColorMap);
	}
}

export struct LightPreparation {
	struct {
		shared_ptr<UploadBuffer<InstanceData>> InInstanceData;
		shared_ptr<UploadBuffer<ObjectData>> InObjectData;
		shared_ptr<DefaultBuffer<RAB_LightInfo>> OutLightInfo;
	} GPUBuffers{};

	LightPreparation(ID3D12Device* pDevice, const shared_ptr<Scene>& scene) noexcept(false) : m_device(pDevice), m_scene(scene) {
		constexpr D3D12_SHADER_BYTECODE ShaderByteCode{ g_LightPreparation_dxil, size(g_LightPreparation_dxil) };
		ThrowIfFailed(pDevice->CreateRootSignature(0, ShaderByteCode.pShaderBytecode, ShaderByteCode.BytecodeLength, IID_PPV_ARGS(&m_rootSignature)));
		const D3D12_COMPUTE_PIPELINE_STATE_DESC computePipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS = ShaderByteCode };
		ThrowIfFailed(pDevice->CreateComputePipelineState(&computePipelineStateDesc, IID_PPV_ARGS(&m_pipelineStateObject)));
	}

	auto GetEmissiveMeshCount() const noexcept { return m_emissiveMeshCount; }
	auto GetEmissiveTriangleCount() const noexcept { return m_emissiveTriangleCount; }
	const auto& GetLightBufferParameters() const noexcept { return m_lightBufferParameters; }

	void CountLights() {
		UINT emissiveMeshCount = 0, emissiveTriangleCount = 0;
		for (const auto& renderObject : m_scene->RenderObjects) {
			if (IsEmissive(renderObject)) {
				emissiveMeshCount++;
				emissiveTriangleCount += static_cast<UINT>(renderObject.Mesh->Indices->GetCount() / 3);
			}
		}
		m_emissiveMeshCount = emissiveMeshCount;
		m_emissiveTriangleCount = emissiveTriangleCount;
		m_lightBufferParameters = {
			.localLightBufferRegion{
				.numLights = emissiveTriangleCount
			},
			.environmentLightParams{
				.lightIndex = RTXDI_INVALID_LIGHT_INDEX
			}
		};
	}

	void PrepareResources(ResourceUploadBatch& resourceUploadBatch, DefaultBuffer<UINT>& lightIndices) {
		vector _lightIndices(m_scene->GetObjectCount(), RTXDI_INVALID_LIGHT_INDEX);
		vector<Task> tasks;
		for (UINT instanceIndex = 0, lightBufferOffset = 0; const auto & renderObject : m_scene->RenderObjects) {
			UINT geometryIndex = 0;
			if (IsEmissive(renderObject)) {
				_lightIndices[m_scene->GetInstanceData()[instanceIndex].FirstGeometryIndex + geometryIndex] = lightBufferOffset;
				const auto triangleCount = static_cast<UINT>(renderObject.Mesh->Indices->GetCount() / 3);
				tasks.emplace_back(Task{
					.InstanceIndex = instanceIndex,
					.GeometryIndex = geometryIndex,
					.TriangleCount = triangleCount,
					.LightBufferOffset = lightBufferOffset
					});
				lightBufferOffset += triangleCount;
			}
			instanceIndex++;
		}
		lightIndices.Upload(resourceUploadBatch, _lightIndices);
		if (m_GPUBuffers.Tasks) m_GPUBuffers.Tasks->Upload(resourceUploadBatch, tasks);
		else m_GPUBuffers.Tasks = make_unique<DefaultBuffer<Task>>(m_device, resourceUploadBatch, tasks);
	}

	void Process(ID3D12GraphicsCommandList* pCommandList, bool ignoreStatic) {
		pCommandList->SetComputeRootSignature(m_rootSignature.Get());
		const struct {
			BOOL IgnoreStatic;
			UINT TaskCount;
		} constants{ ignoreStatic, m_emissiveMeshCount };
		pCommandList->SetComputeRoot32BitConstants(0, 2, &constants, 0);
		pCommandList->SetComputeRootShaderResourceView(1, m_GPUBuffers.Tasks->GetResource()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootShaderResourceView(2, GPUBuffers.InInstanceData->GetResource()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootShaderResourceView(3, GPUBuffers.InObjectData->GetResource()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootUnorderedAccessView(4, GPUBuffers.OutLightInfo->GetResource()->GetGPUVirtualAddress());
		pCommandList->SetPipelineState(m_pipelineStateObject.Get());
		pCommandList->Dispatch((m_emissiveTriangleCount + 255) / 256, 1, 1);
	}

private:
	ID3D12Device* m_device;

	shared_ptr<Scene> m_scene;

	ComPtr<ID3D12RootSignature> m_rootSignature;
	ComPtr<ID3D12PipelineState> m_pipelineStateObject;

	UINT m_emissiveMeshCount{}, m_emissiveTriangleCount{};
	RTXDI_LightBufferParameters m_lightBufferParameters{};

	struct Task { UINT InstanceIndex, GeometryIndex, TriangleCount, LightBufferOffset; };
	struct { unique_ptr<DefaultBuffer<Task>> Tasks; } m_GPUBuffers;
};
