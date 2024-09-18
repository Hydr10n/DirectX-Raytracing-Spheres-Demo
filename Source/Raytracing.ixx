module;

#include "directx/d3dx12.h"

#include "directxtk12/DirectXHelpers.h"

#include "ShaderMake/ShaderBlob.h"

#include "Shaders/Raytracing.dxil.h"

export module Raytracing;

import CommandList;
import DeviceContext;
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

#define MAKE_NAME(Name) static constexpr LPCWSTR Name = L#Name;

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

	struct { GPUBuffer* SceneData, * Camera, * InstanceData, * ObjectData; } GPUBuffers{};

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

	explicit Raytracing(CommandList& commandList) noexcept(false) : m_GPUBuffers{
		.GraphicsSettings = GPUBuffer::CreateConstant<_GraphicsSettings, false>(commandList.GetDeviceContext())
	} {
		const auto& deviceContext = commandList.GetDeviceContext();
		const auto CreateShader = [&](Shader& shader, const ShaderConstant& constant) {
			D3D12_SHADER_BYTECODE shaderBytecode;
			FindPermutationInBlob(g_Raytracing_dxil, size(g_Raytracing_dxil), &constant, 1, &shaderBytecode.pShaderBytecode, &shaderBytecode.BytecodeLength);

			ThrowIfFailed(deviceContext.Device->CreateRootSignature(0, shaderBytecode.pShaderBytecode, shaderBytecode.BytecodeLength, IID_PPV_ARGS(&shader.RootSignature)));

			CD3DX12_STATE_OBJECT_DESC stateObjectDesc(D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE);
			const auto subobject = stateObjectDesc.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();
			subobject->SetDXILLibrary(&shaderBytecode);
			subobject->DefineExport(L"ShaderConfig");
			subobject->DefineExport(L"PipelineConfig");
			subobject->DefineExport(L"GlobalRootSignature");
			subobject->DefineExport(L"RayGeneration");
			ThrowIfFailed(deviceContext.Device->CreateStateObject(stateObjectDesc, IID_PPV_ARGS(&shader.PipelineState)));
			shader.PipelineState->SetName(L"Raytracing");

			ShaderBindingTable::Entry rayGeneration{ L"RayGeneration" };
			vector<ShaderBindingTable::Entry> missEntries, hitGroups;
			ComPtr<ID3D12StateObjectProperties> stateObjectProperties;
			ThrowIfFailed(shader.PipelineState->QueryInterface(IID_PPV_ARGS(&stateObjectProperties)));
			shader.BindingTable = make_unique<ShaderBindingTable>(commandList, stateObjectProperties.Get(), rayGeneration, missEntries, hitGroups);
		};
		CreateShader(m_default, { "DEFAULT", "1" });
		CreateShader(m_SHARCUpdate, { "SHARC_UPDATE", "1" });
		CreateShader(m_SHARCQuery, { "SHARC_QUERY", "1" });
	}

	void SetConstants(const GraphicsSettings& graphicsSettings) noexcept {
		m_renderSize = graphicsSettings.RenderSize;
		m_graphicsSettings = {
			.FrameIndex = graphicsSettings.FrameIndex,
			.Bounces = graphicsSettings.Bounces,
			.SamplesPerPixel = graphicsSettings.SamplesPerPixel,
			.ThroughputThreshold = graphicsSettings.ThroughputThreshold,
			.IsRussianRouletteEnabled = graphicsSettings.IsRussianRouletteEnabled,
			.IsShaderExecutionReorderingEnabled = graphicsSettings.IsShaderExecutionReorderingEnabled,
			.NRD = graphicsSettings.NRD
		};
	}

	void Render(CommandList& commandList, D3D12_GPU_VIRTUAL_ADDRESS topLevelAccelerationStructure) {
		SetConstants(commandList);

		SetShader(commandList, topLevelAccelerationStructure, m_default);

		DispatchRays(commandList, *m_default.BindingTable, m_renderSize);
	}

	void Render(CommandList& commandList, D3D12_GPU_VIRTUAL_ADDRESS topLevelAccelerationStructure, SHARC& SHARC, const SHARCSettings& SHARCSettings) {
		m_graphicsSettings.RTXGI.SHARC = {
			.Capacity = static_cast<UINT>(SHARC.GPUBuffers.HashEntries->GetCapacity()),
			.SceneScale = SHARCSettings.SceneScale,
			.RoughnessThreshold = SHARCSettings.RoughnessThreshold,
			.IsHashGridVisualizationEnabled = SHARCSettings.IsHashGridVisualizationEnabled
		};
		SetConstants(commandList);

		const auto DispatchRays = [&](const Shader& shader, XMUINT2 renderSize) {
			UINT i = SetShader(commandList, topLevelAccelerationStructure, shader);

			commandList->SetComputeRootUnorderedAccessView(i++, SHARC.GPUBuffers.HashEntries->GetNative()->GetGPUVirtualAddress());
			commandList->SetComputeRootUnorderedAccessView(i++, SHARC.GPUBuffers.PreviousVoxelData->GetNative()->GetGPUVirtualAddress());
			commandList->SetComputeRootUnorderedAccessView(i++, SHARC.GPUBuffers.VoxelData->GetNative()->GetGPUVirtualAddress());

			this->DispatchRays(commandList, *shader.BindingTable, renderSize);
		};

		DispatchRays(m_SHARCUpdate, { m_renderSize.x / SHARCSettings.DownscaleFactor, m_renderSize.y / SHARCSettings.DownscaleFactor });

		SHARC.GPUBuffers.Camera = GPUBuffers.Camera;

		SHARC.Process(commandList, SHARCSettings);

		commandList.SetUAVBarrier(*SHARC.GPUBuffers.HashEntries);
		commandList.SetUAVBarrier(*SHARC.GPUBuffers.VoxelData);

		DispatchRays(m_SHARCQuery, m_renderSize);
	}

private:
	struct alignas(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT) _GraphicsSettings {
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

	struct { unique_ptr<GPUBuffer> GraphicsSettings; } m_GPUBuffers;

	struct Shader {
		ComPtr<ID3D12RootSignature> RootSignature;
		ComPtr<ID3D12StateObject> PipelineState;

		unique_ptr<ShaderBindingTable> BindingTable;
	} m_default, m_SHARCUpdate, m_SHARCQuery;

	XMUINT2 m_renderSize{};
	_GraphicsSettings m_graphicsSettings{};

	void SetConstants(CommandList& commandList) {
		commandList.Copy(*m_GPUBuffers.GraphicsSettings, initializer_list{ m_graphicsSettings });
		commandList.SetState(*m_GPUBuffers.GraphicsSettings, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
	}

	UINT SetShader(CommandList& commandList, D3D12_GPU_VIRTUAL_ADDRESS topLevelAccelerationStructure, const Shader& shader) {
		commandList->SetComputeRootSignature(shader.RootSignature.Get());
		commandList->SetPipelineState1(shader.PipelineState.Get());

		commandList.SetState(*GPUBuffers.SceneData, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
		commandList.SetState(*GPUBuffers.Camera, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
		commandList.SetState(*GPUBuffers.InstanceData, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		commandList.SetState(*GPUBuffers.ObjectData, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		commandList.SetState(*Textures.Color, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		commandList.SetState(*Textures.LinearDepth, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		commandList.SetState(*Textures.NormalizedDepth, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		commandList.SetState(*Textures.MotionVectors, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		commandList.SetState(*Textures.BaseColorMetalness, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		commandList.SetState(*Textures.EmissiveColor, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		commandList.SetState(*Textures.Normals, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		commandList.SetState(*Textures.Roughness, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		commandList.SetState(*Textures.NormalRoughness, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		commandList.SetState(*Textures.NoisyDiffuse, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		commandList.SetState(*Textures.NoisySpecular, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		UINT i = 0;
		commandList->SetComputeRootShaderResourceView(i++, topLevelAccelerationStructure);
		commandList->SetComputeRootConstantBufferView(i++, m_GPUBuffers.GraphicsSettings->GetNative()->GetGPUVirtualAddress());
		commandList->SetComputeRootConstantBufferView(i++, GPUBuffers.SceneData->GetNative()->GetGPUVirtualAddress());
		commandList->SetComputeRootConstantBufferView(i++, GPUBuffers.Camera->GetNative()->GetGPUVirtualAddress());
		commandList->SetComputeRootShaderResourceView(i++, GPUBuffers.InstanceData ? GPUBuffers.InstanceData->GetNative()->GetGPUVirtualAddress() : NULL);
		commandList->SetComputeRootShaderResourceView(i++, GPUBuffers.ObjectData ? GPUBuffers.ObjectData->GetNative()->GetGPUVirtualAddress() : NULL);
		commandList->SetComputeRootDescriptorTable(i++, Textures.Color->GetUAVDescriptor());
		commandList->SetComputeRootDescriptorTable(i++, Textures.LinearDepth->GetUAVDescriptor());
		commandList->SetComputeRootDescriptorTable(i++, Textures.NormalizedDepth->GetUAVDescriptor());
		commandList->SetComputeRootDescriptorTable(i++, Textures.MotionVectors->GetUAVDescriptor());
		commandList->SetComputeRootDescriptorTable(i++, Textures.BaseColorMetalness->GetUAVDescriptor());
		commandList->SetComputeRootDescriptorTable(i++, Textures.EmissiveColor->GetUAVDescriptor());
		commandList->SetComputeRootDescriptorTable(i++, Textures.Normals->GetUAVDescriptor());
		commandList->SetComputeRootDescriptorTable(i++, Textures.Roughness->GetUAVDescriptor());
		commandList->SetComputeRootDescriptorTable(i++, Textures.NormalRoughness->GetUAVDescriptor());
		commandList->SetComputeRootDescriptorTable(i++, Textures.NoisyDiffuse->GetUAVDescriptor());
		commandList->SetComputeRootDescriptorTable(i++, Textures.NoisySpecular->GetUAVDescriptor());
		return i;
	}

	void DispatchRays(CommandList& commandList, const ShaderBindingTable& shaderBindingTable, XMUINT2 renderSize) {
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
		commandList->DispatchRays(&dispatchRaysDesc);
	}
};
