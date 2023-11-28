module;

#include "directx/d3dx12.h"

#include <DirectXMath.h>

#include "rtxmu/D3D12AccelStructManager.h"

#include <future>
#include <unordered_map>

#include "Shaders/Raytracing.dxil.h"

export module Raytracing;

import CommandList;
import CommonShaderData;
import ErrorHelpers;
import GPUBuffer;
import NRD;
import RaytracingHelpers;
import Scene;

using namespace DirectX;
using namespace DirectX::RaytracingHelpers;
using namespace ErrorHelpers;
using namespace Microsoft::WRL;
using namespace rtxmu;
using namespace std;

export struct Raytracing {
	struct GlobalResourceDescriptorHeapIndices {
		UINT
			InCamera = ~0u,
			InSceneData = ~0u,
			InInstanceData = ~0u,
			InObjectResourceDescriptorHeapIndices = ~0u, InObjectData = ~0u,
			InEnvironmentLightTexture = ~0u, InEnvironmentTexture = ~0u,
			OutColor = ~0u,
			OutLinearDepth = ~0u, OutNormalizedDepth = ~0u,
			OutMotionVectors = ~0u,
			OutBaseColorMetalness = ~0u,
			OutEmissiveColor = ~0u,
			OutNormalRoughness = ~0u,
			OutNoisyDiffuse = ~0u, OutNoisySpecular = ~0u;
	};

	struct GraphicsSettings {
		XMUINT2 RenderSize;
		UINT FrameIndex, MaxNumberOfBounces, SamplesPerPixel;
		BOOL IsRussianRouletteEnabled;
		NRDDenoiser NRDDenoiser;
		UINT _;
		XMFLOAT4 NRDHitDistanceParameters;
	};

	Raytracing(ID3D12Device5* pDevice, ID3D12CommandQueue* pCommandQueue, const GlobalResourceDescriptorHeapIndices& globalResourceDescriptorHeapIndices, const shared_ptr<Scene>& scene) noexcept(false) :
		m_device(pDevice),
		m_commandQueue(pCommandQueue),
		m_commandList(pDevice),
		m_GPUBuffers{
			.GlobalResourceDescriptorHeapIndices = ConstantBuffer<GlobalResourceDescriptorHeapIndices>(pDevice, initializer_list{ globalResourceDescriptorHeapIndices }),
			.GraphicsSettings = ConstantBuffer<GraphicsSettings>(pDevice)
		},
		m_scene(scene) {
		constexpr D3D12_SHADER_BYTECODE shaderByteCode{ g_Raytracing_dxil, size(g_Raytracing_dxil) };
		ThrowIfFailed(pDevice->CreateRootSignature(0, shaderByteCode.pShaderBytecode, shaderByteCode.BytecodeLength, IID_PPV_ARGS(&m_rootSignature)));
		const D3D12_COMPUTE_PIPELINE_STATE_DESC computePipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS = shaderByteCode };
		ThrowIfFailed(pDevice->CreateComputePipelineState(&computePipelineStateDesc, IID_PPV_ARGS(&m_pipelineState)));

		CreateAccelerationStructures(false);
	}

	void SetConstants(const GraphicsSettings& graphicsSettings) noexcept { m_GPUBuffers.GraphicsSettings.GetData() = graphicsSettings; }

	void UpdateAccelerationStructures(bool updateOnly = true) { CreateAccelerationStructures(updateOnly); }

	void Render(ID3D12GraphicsCommandList* pCommandList) {
		pCommandList->SetComputeRootSignature(m_rootSignature.Get());
		pCommandList->SetComputeRootShaderResourceView(0, m_topLevelAccelerationStructure->GetBuffer()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootConstantBufferView(1, m_GPUBuffers.GlobalResourceDescriptorHeapIndices.GetResource()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootConstantBufferView(2, m_GPUBuffers.GraphicsSettings.GetResource()->GetGPUVirtualAddress());
		pCommandList->SetPipelineState(m_pipelineState.Get());
		const auto renderSize = m_GPUBuffers.GraphicsSettings.GetData().RenderSize;
		pCommandList->Dispatch((renderSize.x + 15) / 16, (renderSize.y + 15) / 16, 1);
	}

private:
	ID3D12Device5* m_device;
	ID3D12CommandQueue* m_commandQueue;
	CommandList<ID3D12GraphicsCommandList4> m_commandList;

	struct {
		ConstantBuffer<GlobalResourceDescriptorHeapIndices> GlobalResourceDescriptorHeapIndices;
		ConstantBuffer<GraphicsSettings> GraphicsSettings;
	} m_GPUBuffers;

	shared_ptr<Scene> m_scene;

	ComPtr<ID3D12RootSignature> m_rootSignature;
	ComPtr<ID3D12PipelineState> m_pipelineState;

	unique_ptr<DxAccelStructManager> m_accelerationStructureManager;
	unordered_map<shared_ptr<Mesh>, uint64_t> m_bottomLevelAccelerationStructureIDs;
	unique_ptr<TopLevelAccelerationStructure> m_topLevelAccelerationStructure;

	void CreateAccelerationStructures(bool updateOnly) {
		const auto pCommandList = m_commandList.GetNative();

		vector<uint64_t> newBottomLevelAccelerationStructureIDs;

		{
			m_commandList.Begin();

			if (!updateOnly) {
				m_bottomLevelAccelerationStructureIDs = {};

				m_accelerationStructureManager = make_unique<DxAccelStructManager>(m_device);
				m_accelerationStructureManager->Initialize();
			}

			vector<vector<D3D12_RAYTRACING_GEOMETRY_DESC>> geometryDescs;
			vector<D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS> newBuildBottomLevelAccelerationStructureInputs;
			vector<shared_ptr<Mesh>> newMeshes;

			for (const auto& renderObject : m_scene->RenderObjects) {
				const auto& mesh = renderObject.Mesh;
				if (const auto [first, second] = m_bottomLevelAccelerationStructureIDs.try_emplace(mesh); second) {
					auto& _geometryDescs = geometryDescs.emplace_back(initializer_list{ CreateGeometryDesc(*mesh->Vertices, *mesh->Indices) });
					newBuildBottomLevelAccelerationStructureInputs.emplace_back(D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS{
						.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL,
						.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION,
						.NumDescs = static_cast<UINT>(size(_geometryDescs)),
						.pGeometryDescs = data(_geometryDescs)
						});
					newMeshes.emplace_back(mesh);
				}
			}

			if (!empty(newBuildBottomLevelAccelerationStructureInputs)) {
				m_accelerationStructureManager->PopulateBuildCommandList(pCommandList, data(newBuildBottomLevelAccelerationStructureInputs), size(newBuildBottomLevelAccelerationStructureInputs), newBottomLevelAccelerationStructureIDs);
				for (size_t i = 0; const auto & meshNode : newMeshes) m_bottomLevelAccelerationStructureIDs[meshNode] = newBottomLevelAccelerationStructureIDs[i++];
				m_accelerationStructureManager->PopulateUAVBarriersCommandList(pCommandList, newBottomLevelAccelerationStructureIDs);
				m_accelerationStructureManager->PopulateCompactionSizeCopiesCommandList(pCommandList, newBottomLevelAccelerationStructureIDs);
			}

			m_commandList.End(m_commandQueue).get();
		}

		{
			m_commandList.Begin();

			if (!empty(newBottomLevelAccelerationStructureIDs)) m_accelerationStructureManager->PopulateCompactionCommandList(pCommandList, newBottomLevelAccelerationStructureIDs);

			if (!updateOnly) {
				m_topLevelAccelerationStructure = make_unique<TopLevelAccelerationStructure>(m_device, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE);
			}
			vector<D3D12_RAYTRACING_INSTANCE_DESC> instanceDescs;
			instanceDescs.reserve(m_topLevelAccelerationStructure->GetDescCount());
			for (size_t objectIndex = 0; const auto & renderObject : m_scene->RenderObjects) {
				auto& instanceDesc = instanceDescs.emplace_back(D3D12_RAYTRACING_INSTANCE_DESC{
					.InstanceID = static_cast<UINT>(objectIndex),
					.InstanceMask = renderObject.IsVisible ? ~0u : 0,
					.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE | D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_FRONT_COUNTERCLOCKWISE,
					.AccelerationStructure = m_accelerationStructureManager->GetAccelStructGPUVA(m_bottomLevelAccelerationStructureIDs.at(renderObject.Mesh))
					});
				XMStoreFloat3x4(reinterpret_cast<XMFLOAT3X4*>(instanceDesc.Transform), Scene::Transform(*renderObject.Shape));
				objectIndex++;
			}
			m_topLevelAccelerationStructure->Build(pCommandList, instanceDescs, updateOnly);

			m_commandList.End(m_commandQueue).get();
		}

		if (!empty(newBottomLevelAccelerationStructureIDs)) m_accelerationStructureManager->GarbageCollection(newBottomLevelAccelerationStructureIDs);
	}
};
