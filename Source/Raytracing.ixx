module;

#include "directx/d3dx12.h"

#include "directxtk12/DirectXHelpers.h"

#include "ShaderMake/ShaderBlob.h"

#include "Shaders/Raytracing.dxil.h"

export module Raytracing;

import CommandList;
import Denoiser;
import DeviceContext;
import ErrorHelpers;
import GPUBuffer;
import RaytracingHelpers;
import RTXGI;
import Texture;

using namespace DirectX;
using namespace DirectX::RaytracingHelpers;
using namespace ErrorHelpers;
using namespace Microsoft::WRL;
using namespace ShaderMake;
using namespace std;

export struct Raytracing {
	struct GraphicsSettings {
		XMUINT2 RenderSize;
		UINT FrameIndex, Bounces, SamplesPerPixel;
		float ThroughputThreshold = 1e-3f;
		BOOL IsRussianRouletteEnabled, IsShaderExecutionReorderingEnabled, IsDIEnabled;
		DenoisingSettings Denoising;
	};

	struct SHARCSettings : SHARC::Constants {
		UINT DownscaleFactor;
		float RoughnessThreshold;
		BOOL IsHashGridVisualizationEnabled;
	};

	struct { GPUBuffer* SceneData, * Camera, * ObjectData; } GPUBuffers{};

	struct {
		Texture
			* Position,
			* FlatNormal,
			* GeometricNormal,
			* LinearDepth,
			* BaseColorMetalness,
			* NormalRoughness,
			* Transmission,
			* IOR,
			* LightRadiance,
			* Radiance,
			* Diffuse,
			* Specular,
			* SpecularHitDistance;
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
		m_graphicsSettings = {
			.RenderSize = graphicsSettings.RenderSize,
			.FrameIndex = graphicsSettings.FrameIndex,
			.Bounces = graphicsSettings.Bounces,
			.SamplesPerPixel = graphicsSettings.SamplesPerPixel,
			.ThroughputThreshold = graphicsSettings.ThroughputThreshold,
			.IsRussianRouletteEnabled = graphicsSettings.IsRussianRouletteEnabled,
			.IsShaderExecutionReorderingEnabled = graphicsSettings.IsShaderExecutionReorderingEnabled,
			.IsDIEnabled = graphicsSettings.IsDIEnabled,
			.Denoising = graphicsSettings.Denoising
		};
	}

	void Render(CommandList& commandList, D3D12_GPU_VIRTUAL_ADDRESS topLevelAccelerationStructure) {
		SetConstants(commandList);

		SetShader(commandList, topLevelAccelerationStructure, m_default);

		DispatchRays(commandList, *m_default.BindingTable, m_graphicsSettings.RenderSize);
	}

	void Render(CommandList& commandList, D3D12_GPU_VIRTUAL_ADDRESS topLevelAccelerationStructure, SHARC& SHARC, const SHARCSettings& SHARCSettings) {
		m_graphicsSettings.RTXGI.SHARC = {
			.Capacity = static_cast<UINT>(SHARC.GPUBuffers.HashEntries->GetCapacity()),
			.SceneScale = SHARCSettings.SceneScale,
			.RoughnessThreshold = SHARCSettings.RoughnessThreshold,
			.IsAntiFileflyEnabled = SHARCSettings.IsAntiFireflyEnabled,
			.IsHashGridVisualizationEnabled = SHARCSettings.IsHashGridVisualizationEnabled
		};
		SetConstants(commandList);

		const auto DispatchRays = [&](const Shader& shader, XMUINT2 renderSize) {
			auto i = SetShader(commandList, topLevelAccelerationStructure, shader);

			commandList->SetComputeRootUnorderedAccessView(i++, SHARC.GPUBuffers.HashEntries->GetNative()->GetGPUVirtualAddress());
			commandList->SetComputeRootUnorderedAccessView(i++, SHARC.GPUBuffers.PreviousVoxelData->GetNative()->GetGPUVirtualAddress());
			commandList->SetComputeRootUnorderedAccessView(i++, SHARC.GPUBuffers.VoxelData->GetNative()->GetGPUVirtualAddress());

			this->DispatchRays(commandList, *shader.BindingTable, renderSize);
		};

		DispatchRays(m_SHARCUpdate, { m_graphicsSettings.RenderSize.x / SHARCSettings.DownscaleFactor, m_graphicsSettings.RenderSize.y / SHARCSettings.DownscaleFactor });

		SHARC.GPUBuffers.Camera = GPUBuffers.Camera;

		SHARC.Process(commandList, SHARCSettings);

		commandList.SetUAVBarrier(*SHARC.GPUBuffers.HashEntries);
		commandList.SetUAVBarrier(*SHARC.GPUBuffers.VoxelData);

		DispatchRays(m_SHARCQuery, m_graphicsSettings.RenderSize);
	}

private:
	struct alignas(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT) _GraphicsSettings {
		XMUINT2 RenderSize;
		UINT FrameIndex, Bounces, SamplesPerPixel;
		float ThroughputThreshold;
		BOOL IsRussianRouletteEnabled, IsShaderExecutionReorderingEnabled, IsDIEnabled;
		XMUINT3 _;
		struct {
			struct {
				UINT Capacity;
				float SceneScale, RoughnessThreshold;
				BOOL IsAntiFileflyEnabled, IsHashGridVisualizationEnabled;
				XMUINT3 _;
			} SHARC;
		} RTXGI;
		DenoisingSettings Denoising;
	};

	struct { unique_ptr<GPUBuffer> GraphicsSettings; } m_GPUBuffers;

	struct Shader {
		ComPtr<ID3D12RootSignature> RootSignature;
		ComPtr<ID3D12StateObject> PipelineState;

		unique_ptr<ShaderBindingTable> BindingTable;
	} m_default, m_SHARCUpdate, m_SHARCQuery;

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
		commandList.SetState(*GPUBuffers.ObjectData, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		commandList.SetState(*Textures.Position, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		commandList.SetState(*Textures.FlatNormal, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		commandList.SetState(*Textures.GeometricNormal, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		commandList.SetState(*Textures.LinearDepth, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		commandList.SetState(*Textures.BaseColorMetalness, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		commandList.SetState(*Textures.NormalRoughness, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		commandList.SetState(*Textures.Transmission, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		commandList.SetState(*Textures.IOR, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		commandList.SetState(*Textures.LightRadiance, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		commandList.SetState(*Textures.Radiance, &shader == &m_SHARCUpdate ? D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		UINT i = 0;
		commandList->SetComputeRootShaderResourceView(i++, topLevelAccelerationStructure);
		commandList->SetComputeRootConstantBufferView(i++, m_GPUBuffers.GraphicsSettings->GetNative()->GetGPUVirtualAddress());
		commandList->SetComputeRootConstantBufferView(i++, GPUBuffers.SceneData->GetNative()->GetGPUVirtualAddress());
		commandList->SetComputeRootConstantBufferView(i++, GPUBuffers.Camera->GetNative()->GetGPUVirtualAddress());
		commandList->SetComputeRootShaderResourceView(i++, GPUBuffers.ObjectData->GetNative()->GetGPUVirtualAddress());
		commandList->SetComputeRootDescriptorTable(i++, Textures.Position->GetSRVDescriptor());
		commandList->SetComputeRootDescriptorTable(i++, Textures.FlatNormal->GetSRVDescriptor());
		commandList->SetComputeRootDescriptorTable(i++, Textures.GeometricNormal->GetSRVDescriptor());
		commandList->SetComputeRootDescriptorTable(i++, Textures.LinearDepth->GetSRVDescriptor());
		commandList->SetComputeRootDescriptorTable(i++, Textures.BaseColorMetalness->GetSRVDescriptor());
		commandList->SetComputeRootDescriptorTable(i++, Textures.NormalRoughness->GetSRVDescriptor());
		commandList->SetComputeRootDescriptorTable(i++, Textures.Transmission->GetSRVDescriptor());
		commandList->SetComputeRootDescriptorTable(i++, Textures.IOR->GetSRVDescriptor());
		commandList->SetComputeRootDescriptorTable(i++, Textures.LightRadiance->GetSRVDescriptor());
		commandList->SetComputeRootDescriptorTable(i++, &shader == &m_SHARCUpdate ? Textures.Radiance->GetSRVDescriptor() : Textures.Radiance->GetUAVDescriptor());
		if (Textures.Diffuse) {
			commandList.SetState(*Textures.Diffuse, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			commandList->SetComputeRootDescriptorTable(i, Textures.Diffuse->GetUAVDescriptor());
		}
		i++;
		if (Textures.Specular) {
			commandList.SetState(*Textures.Specular, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			commandList->SetComputeRootDescriptorTable(i, Textures.Specular->GetUAVDescriptor());
		}
		i++;
		if (Textures.SpecularHitDistance) {
			commandList.SetState(*Textures.SpecularHitDistance, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			commandList->SetComputeRootDescriptorTable(i, Textures.SpecularHitDistance->GetUAVDescriptor());
		}
		i++;
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
