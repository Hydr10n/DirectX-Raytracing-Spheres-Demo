module;

#include "directx/d3dx12.h"

#include "directxtk12/DirectXHelpers.h"

#include "ShaderMake/ShaderBlob.h"

#include "Shaders/Raytracing.dxil.h"

export module Raytracing;

import Camera;
import CommonShaderData;
import ErrorHelpers;
import GPUBuffer;
import NRD;
import RaytracingHelpers;
import RTXGI;
import Texture;

using namespace DirectX;
using namespace DirectX::RaytracingHelpers;
using namespace ErrorHelpers;
using namespace Microsoft::WRL;
using namespace ShaderMake;
using namespace std;

#define MAKE_NAME(name) static constexpr LPCWSTR name = L#name;

export struct Raytracing {
	struct GraphicsSettings {
		XMUINT2 RenderSize;
		UINT FrameIndex, Bounces, SamplesPerPixel;
		float ThroughputThreshold = 1e-3f;
		BOOL IsRussianRouletteEnabled, IsShaderExecutionReorderingEnabled;
		NRDSettings NRD;
	};

	struct SHARCSettings : SHARC::Constants {
		UINT DownscaleFactor;
		float RoughnessThreshold;
		BOOL IsHashGridVisualizationEnabled;
	};

	struct {
		ConstantBuffer<SceneData>* SceneData;
		ConstantBuffer<Camera>* Camera;
		UploadBuffer<InstanceData>* InstanceData;
		UploadBuffer<ObjectData>* ObjectData;
	} GPUBuffers{};

	struct {
		Texture
			* Color,
			* LinearDepth,
			* NormalizedDepth,
			* MotionVectors,
			* BaseColorMetalness,
			* EmissiveColor,
			* Normals,
			* Roughness,
			* NormalRoughness,
			* NoisyDiffuse,
			* NoisySpecular;
	} Textures{};

	explicit Raytracing(ID3D12Device5* pDevice) noexcept(false) :
		m_GPUBuffers{ .GraphicsSettings = ConstantBuffer<_GraphicsSettings>(pDevice) } {
		const auto CreateShader = [&](Shader& shader, const ShaderConstant& constant) {
			D3D12_SHADER_BYTECODE shaderBytecode;
			FindPermutationInBlob(g_Raytracing_dxil, size(g_Raytracing_dxil), &constant, 1, &shaderBytecode.pShaderBytecode, &shaderBytecode.BytecodeLength);

			ThrowIfFailed(pDevice->CreateRootSignature(0, shaderBytecode.pShaderBytecode, shaderBytecode.BytecodeLength, IID_PPV_ARGS(&shader.RootSignature)));

			CD3DX12_STATE_OBJECT_DESC stateObjectDesc(D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE);
			const auto subobject = stateObjectDesc.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();
			subobject->SetDXILLibrary(&shaderBytecode);
			subobject->DefineExport(L"ShaderConfig");
			subobject->DefineExport(L"PipelineConfig");
			subobject->DefineExport(L"GlobalRootSignature");
			subobject->DefineExport(L"RayGeneration");
			ThrowIfFailed(pDevice->CreateStateObject(stateObjectDesc, IID_PPV_ARGS(&shader.PipelineState)));
			shader.PipelineState->SetName(L"Raytracing");

			vector<ShaderBindingTable::Entry>
				rayGenerationEntries{ { L"RayGeneration" } },
				missEntries,
				hitGroups;
			ComPtr<ID3D12StateObjectProperties> stateObjectProperties;
			ThrowIfFailed(shader.PipelineState->QueryInterface(IID_PPV_ARGS(&stateObjectProperties)));
			shader.BindingTable = make_unique<ShaderBindingTable>(pDevice, stateObjectProperties.Get(), rayGenerationEntries, missEntries, hitGroups);
		};
		CreateShader(m_default, { "DEFAULT", "1" });
		CreateShader(m_SHARCUpdate, { "SHARC_UPDATE", "1" });
		CreateShader(m_SHARCQuery, { "SHARC_QUERY", "1" });
	}

	void SetConstants(const GraphicsSettings& graphicsSettings) {
		auto& _graphicsSettings = m_GPUBuffers.GraphicsSettings.At(0);
		_graphicsSettings.FrameIndex = graphicsSettings.FrameIndex;
		_graphicsSettings.Bounces = graphicsSettings.Bounces;
		_graphicsSettings.SamplesPerPixel = graphicsSettings.SamplesPerPixel;
		_graphicsSettings.ThroughputThreshold = graphicsSettings.ThroughputThreshold;
		_graphicsSettings.IsRussianRouletteEnabled = graphicsSettings.IsRussianRouletteEnabled;
		_graphicsSettings.IsShaderExecutionReorderingEnabled = graphicsSettings.IsShaderExecutionReorderingEnabled;
		_graphicsSettings.NRD = graphicsSettings.NRD;

		m_renderSize = graphicsSettings.RenderSize;
	}

	void Render(ID3D12GraphicsCommandList4* pCommandList, const TopLevelAccelerationStructure& scene) {
		const ScopedBarrier scopedBarrier = CreateScopedBarrier(pCommandList);

		SetShader(pCommandList, scene, m_default);

		DispatchRays(pCommandList, *m_default.BindingTable, m_renderSize);
	}

	void Render(ID3D12GraphicsCommandList4* pCommandList, const TopLevelAccelerationStructure& scene, SHARC& SHARC, const SHARCSettings& SHARCSettings) {
		const ScopedBarrier scopedBarrier = CreateScopedBarrier(pCommandList);

		m_GPUBuffers.GraphicsSettings.At(0).RTXGI.SHARC = {
			.Capacity = static_cast<UINT>(SHARC.GPUBuffers.HashEntries->GetCount()),
			.SceneScale = SHARCSettings.SceneScale,
			.RoughnessThreshold = SHARCSettings.RoughnessThreshold,
			.IsHashGridVisualizationEnabled = SHARCSettings.IsHashGridVisualizationEnabled
		};

		const auto DispatchRays = [&](const Shader& shader, XMUINT2 renderSize) {
			UINT i = SetShader(pCommandList, scene, shader);

			pCommandList->SetComputeRootUnorderedAccessView(i++, SHARC.GPUBuffers.HashEntries->GetNative()->GetGPUVirtualAddress());
			pCommandList->SetComputeRootUnorderedAccessView(i++, SHARC.GPUBuffers.PreviousVoxelData->GetNative()->GetGPUVirtualAddress());
			pCommandList->SetComputeRootUnorderedAccessView(i++, SHARC.GPUBuffers.VoxelData->GetNative()->GetGPUVirtualAddress());

			this->DispatchRays(pCommandList, *shader.BindingTable, renderSize);
		};

		DispatchRays(m_SHARCUpdate, { m_renderSize.x / SHARCSettings.DownscaleFactor, m_renderSize.y / SHARCSettings.DownscaleFactor });

		SHARC.GPUBuffers.Camera = GPUBuffers.Camera;

		SHARC.Process(pCommandList, SHARCSettings);

		{
			const auto barriers = {
				CD3DX12_RESOURCE_BARRIER::UAV(*SHARC.GPUBuffers.HashEntries),
				CD3DX12_RESOURCE_BARRIER::UAV(*SHARC.GPUBuffers.VoxelData)
			};
			pCommandList->ResourceBarrier(static_cast<UINT>(size(barriers)), data(barriers));
		}

		DispatchRays(m_SHARCQuery, m_renderSize);
	}

private:
	struct _GraphicsSettings {
		UINT FrameIndex, Bounces, SamplesPerPixel;
		float ThroughputThreshold;
		BOOL IsRussianRouletteEnabled, IsShaderExecutionReorderingEnabled;
		XMUINT2 _;
		struct {
			struct {
				UINT Capacity;
				float SceneScale, RoughnessThreshold;
				BOOL IsHashGridVisualizationEnabled;
			} SHARC;
		} RTXGI;
		NRDSettings NRD;
	};

	struct { ConstantBuffer<_GraphicsSettings> GraphicsSettings; } m_GPUBuffers;

	struct Shader {
		ComPtr<ID3D12RootSignature> RootSignature;
		ComPtr<ID3D12StateObject> PipelineState;

		unique_ptr<ShaderBindingTable> BindingTable;
	} m_default, m_SHARCUpdate, m_SHARCQuery;

	XMUINT2 m_renderSize{};

	ScopedBarrier CreateScopedBarrier(ID3D12GraphicsCommandList* pCommandList) {
		return ScopedBarrier(
			pCommandList,
			{
				Textures.Color->TransitionBarrier(D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				Textures.LinearDepth->TransitionBarrier(D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				Textures.NormalizedDepth->TransitionBarrier(D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				Textures.MotionVectors->TransitionBarrier(D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				Textures.BaseColorMetalness->TransitionBarrier(D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				Textures.EmissiveColor->TransitionBarrier(D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				Textures.Normals->TransitionBarrier(D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				Textures.Roughness->TransitionBarrier(D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				Textures.NormalRoughness->TransitionBarrier(D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				Textures.NoisyDiffuse->TransitionBarrier(D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				Textures.NoisySpecular->TransitionBarrier(D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
			}
		);
	}

	UINT SetShader(ID3D12GraphicsCommandList4* pCommandList, const TopLevelAccelerationStructure& scene, const Shader& shader) {
		pCommandList->SetComputeRootSignature(shader.RootSignature.Get());
		pCommandList->SetPipelineState1(shader.PipelineState.Get());

		UINT i = 0;
		pCommandList->SetComputeRootShaderResourceView(i++, scene.GetBuffer()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootConstantBufferView(i++, m_GPUBuffers.GraphicsSettings->GetGPUVirtualAddress());
		pCommandList->SetComputeRootConstantBufferView(i++, GPUBuffers.SceneData->GetNative()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootConstantBufferView(i++, GPUBuffers.Camera->GetNative()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootShaderResourceView(i++, GPUBuffers.InstanceData ? GPUBuffers.InstanceData->GetNative()->GetGPUVirtualAddress() : NULL);
		pCommandList->SetComputeRootShaderResourceView(i++, GPUBuffers.ObjectData ? GPUBuffers.ObjectData->GetNative()->GetGPUVirtualAddress() : NULL);
		pCommandList->SetComputeRootDescriptorTable(i++, Textures.Color->GetUAVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(i++, Textures.LinearDepth->GetUAVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(i++, Textures.NormalizedDepth->GetUAVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(i++, Textures.MotionVectors->GetUAVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(i++, Textures.BaseColorMetalness->GetUAVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(i++, Textures.EmissiveColor->GetUAVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(i++, Textures.Normals->GetUAVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(i++, Textures.Roughness->GetUAVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(i++, Textures.NormalRoughness->GetUAVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(i++, Textures.NoisyDiffuse->GetUAVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(i++, Textures.NoisySpecular->GetUAVDescriptor().GPUHandle);
		return i;
	}

	void DispatchRays(ID3D12GraphicsCommandList4* pCommandList, const ShaderBindingTable& shaderBindingTable, XMUINT2 renderSize) {
		const D3D12_DISPATCH_RAYS_DESC dispatchRaysDesc{
			.RayGenerationShaderRecord{
				.StartAddress = shaderBindingTable.GetRayGenerationAddress(),
				.SizeInBytes = shaderBindingTable.GetRayGenerationSize()
			},
			.MissShaderTable{
				.StartAddress = shaderBindingTable.GetMissAddress(),
				.SizeInBytes = shaderBindingTable.GetMissSize(),
				.StrideInBytes = shaderBindingTable.GetMissStride()
			},
			.HitGroupTable{
				.StartAddress = shaderBindingTable.GetHitGroupAddress(),
				.SizeInBytes = shaderBindingTable.GetHitGroupSize(),
				.StrideInBytes = shaderBindingTable.GetHitGroupStride()
			},
			.Width = renderSize.x,
			.Height = renderSize.y,
			.Depth = 1
		};
		pCommandList->DispatchRays(&dispatchRaysDesc);
	}
};
