module;

#include <memory>

#include <DirectXMath.h>

#include "directxtk12/DirectXHelpers.h"

#include "rtxdi/RtxdiParameters.h"

#include "Shaders/LightPreparation.dxil.h"

export module LightPreparation;

import CommandList;
import DeviceContext;
import ErrorHelpers;
import GPUBuffer;
import Model;
import Scene;
import Texture;

using namespace DirectX;
using namespace ErrorHelpers;
using namespace Microsoft::WRL;
using namespace std;

export struct LightPreparation {
	struct { GPUBuffer* InstanceData, * ObjectData, * LightInfo; } GPUBuffers{};

	struct { Texture* LocalLightPDF; } Textures{};

	explicit LightPreparation(const DeviceContext& deviceContext) noexcept(false) {
		constexpr D3D12_SHADER_BYTECODE ShaderByteCode{ g_LightPreparation_dxil, size(g_LightPreparation_dxil) };

		ThrowIfFailed(deviceContext.Device->CreateRootSignature(0, ShaderByteCode.pShaderBytecode, ShaderByteCode.BytecodeLength, IID_PPV_ARGS(&m_rootSignature)));

		const D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS = ShaderByteCode };
		ThrowIfFailed(deviceContext.Device->CreateComputePipelineState(&pipelineStateDesc, IID_PPV_ARGS(&m_pipelineState)));
		m_pipelineState->SetName(L"LightPreparation");
	}

	void SetScene(const Scene* pScene) {
		m_scene = pScene;
		CountLights();
	}

	auto GetEmissiveMeshCount() const noexcept { return m_emissiveMeshCount; }
	auto GetEmissiveTriangleCount() const noexcept { return m_emissiveTriangleCount; }
	const auto& GetLightBufferParameters() const noexcept { return m_lightBufferParameters; }

	void CountLights() {
		UINT emissiveMeshCount = 0, emissiveTriangleCount = 0;
		for (const auto& renderObject : m_scene->RenderObjects) {
			if (IsEmissive(renderObject)) {
				emissiveMeshCount++;
				emissiveTriangleCount += static_cast<UINT>(renderObject.Mesh->Indices->GetCapacity()) / 3;
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

	void PrepareResources(CommandList& commandList, GPUBuffer& lightIndices) {
		vector _lightIndices(m_scene->GetObjectCount(), RTXDI_INVALID_LIGHT_INDEX);
		vector<Task> tasks;
		for (UINT instanceIndex = 0, lightBufferOffset = 0; const auto & renderObject : m_scene->RenderObjects) {
			UINT geometryIndex = 0;
			if (IsEmissive(renderObject)) {
				_lightIndices[m_scene->GetInstanceData()[instanceIndex].FirstGeometryIndex + geometryIndex] = lightBufferOffset;
				const auto triangleCount = static_cast<UINT>(renderObject.Mesh->Indices->GetCapacity()) / 3;
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

		commandList.Copy(lightIndices, _lightIndices);

		if (const auto size = ::size(tasks); !m_GPUBuffers.Tasks || size > m_GPUBuffers.Tasks->GetCapacity()) {
			m_GPUBuffers.Tasks = GPUBuffer::CreateDefault<Task>(commandList.GetDeviceContext(), size);
		}
		commandList.Copy(*m_GPUBuffers.Tasks, tasks);
	}

	void Process(CommandList& commandList) {
		commandList->SetComputeRootSignature(m_rootSignature.Get());
		commandList->SetPipelineState(m_pipelineState.Get());

		commandList.SetState(*m_GPUBuffers.Tasks, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		commandList.SetState(*GPUBuffers.InstanceData, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		commandList.SetState(*GPUBuffers.ObjectData, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		commandList.SetState(*GPUBuffers.LightInfo, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		commandList.SetState(*Textures.LocalLightPDF, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		commandList->SetComputeRoot32BitConstant(0, m_emissiveMeshCount, 0);
		commandList->SetComputeRootShaderResourceView(1, m_GPUBuffers.Tasks->GetNative()->GetGPUVirtualAddress());
		commandList->SetComputeRootShaderResourceView(2, GPUBuffers.InstanceData->GetNative()->GetGPUVirtualAddress());
		commandList->SetComputeRootShaderResourceView(3, GPUBuffers.ObjectData->GetNative()->GetGPUVirtualAddress());
		commandList->SetComputeRootUnorderedAccessView(4, GPUBuffers.LightInfo->GetNative()->GetGPUVirtualAddress());
		commandList->SetComputeRootDescriptorTable(5, Textures.LocalLightPDF->GetUAVDescriptor());

		commandList->Dispatch((m_emissiveTriangleCount + 255) / 256, 1, 1);
	}

private:
	ComPtr<ID3D12RootSignature> m_rootSignature;
	ComPtr<ID3D12PipelineState> m_pipelineState;

	const Scene* m_scene{};

	UINT m_emissiveMeshCount{}, m_emissiveTriangleCount{};
	RTXDI_LightBufferParameters m_lightBufferParameters{};

	struct Task { UINT InstanceIndex, GeometryIndex, TriangleCount, LightBufferOffset; };
	struct { unique_ptr<GPUBuffer> Tasks; } m_GPUBuffers;

	constexpr bool IsEmissive(const RenderObject& renderObject) {
		constexpr auto Max = [](const XMFLOAT3& value) { return max(max(value.x, value.y), value.z); };
		return Max(renderObject.Material.EmissiveColor) > 0 || renderObject.Textures[to_underlying(TextureMapType::EmissiveColor)];
	}
};
