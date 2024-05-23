module;

#include "directx/d3dx12.h"

#include "directxtk12/DirectXHelpers.h"

#include "Shaders/Raytracing.dxil.h"

export module Raytracing;

import Camera;
import CommonShaderData;
import ErrorHelpers;
import GPUBuffer;
import NRD;
import RaytracingHelpers;
import Texture;

using namespace DirectX;
using namespace DirectX::RaytracingHelpers;
using namespace ErrorHelpers;
using namespace Microsoft::WRL;
using namespace std;

#define MAKE_NAME(name) static constexpr LPCWSTR name = L#name;

export struct Raytracing {
	struct GraphicsSettings {
		XMUINT2 RenderSize;
		UINT FrameIndex, Bounces, SamplesPerPixel, _;
		BOOL IsRussianRouletteEnabled, IsShaderExecutionReorderingEnabled;
		NRDSettings NRD;
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
	} RenderTextures{};

	explicit Raytracing(ID3D12Device5* pDevice) noexcept(false) : m_GPUBuffers{ .GraphicsSettings = ConstantBuffer<GraphicsSettings>(pDevice) } {
		constexpr D3D12_SHADER_BYTECODE ShaderByteCode{ g_Raytracing_dxil, size(g_Raytracing_dxil) };

		ThrowIfFailed(pDevice->CreateRootSignature(0, ShaderByteCode.pShaderBytecode, ShaderByteCode.BytecodeLength, IID_PPV_ARGS(&m_rootSignature)));

		CD3DX12_STATE_OBJECT_DESC stateObjectDesc(D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE);
		const auto subobject = stateObjectDesc.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();
		subobject->SetDXILLibrary(&ShaderByteCode);
		subobject->DefineExport(L"ShaderConfig");
		subobject->DefineExport(L"PipelineConfig");
		subobject->DefineExport(L"GlobalRootSignature");
		subobject->DefineExport(ShaderEntryNames::RayGeneration);
		ThrowIfFailed(pDevice->CreateStateObject(stateObjectDesc, IID_PPV_ARGS(&m_pipelineState)));
		m_pipelineState->SetName(L"Raytracing");

		vector<ShaderBindingTable::Entry>
			rayGenerationEntries{ { ShaderEntryNames::RayGeneration } },
			missEntries,
			hitGroupEntries;
		ComPtr<ID3D12StateObjectProperties> stateObjectProperties;
		ThrowIfFailed(m_pipelineState->QueryInterface(IID_PPV_ARGS(&stateObjectProperties)));
		m_shaderBindingTable = make_unique<ShaderBindingTable>(pDevice, stateObjectProperties.Get(), rayGenerationEntries, missEntries, hitGroupEntries);
	}

	void SetConstants(const GraphicsSettings& graphicsSettings) noexcept {
		m_GPUBuffers.GraphicsSettings.At(0) = graphicsSettings;
		m_renderSize = graphicsSettings.RenderSize;
	}

	void Render(ID3D12GraphicsCommandList4* pCommandList, const TopLevelAccelerationStructure& scene) {
		const ScopedBarrier scopedBarrier(
			pCommandList,
			{
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.Color, RenderTextures.Color->GetState(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.LinearDepth, RenderTextures.LinearDepth->GetState(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.NormalizedDepth, RenderTextures.NormalizedDepth->GetState(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.MotionVectors, RenderTextures.MotionVectors->GetState(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.BaseColorMetalness, RenderTextures.BaseColorMetalness->GetState(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.EmissiveColor, RenderTextures.EmissiveColor->GetState(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.Normals, RenderTextures.Normals->GetState(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.Roughness, RenderTextures.Roughness->GetState(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.NormalRoughness, RenderTextures.NormalRoughness->GetState(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.NoisyDiffuse, RenderTextures.NoisyDiffuse->GetState(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.NoisySpecular, RenderTextures.NoisySpecular->GetState(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
			}
		);

		pCommandList->SetComputeRootSignature(m_rootSignature.Get());
		pCommandList->SetPipelineState1(m_pipelineState.Get());

		UINT i = 0;
		pCommandList->SetComputeRootShaderResourceView(i++, scene.GetBuffer()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootConstantBufferView(i++, m_GPUBuffers.GraphicsSettings->GetGPUVirtualAddress());
		pCommandList->SetComputeRootConstantBufferView(i++, GPUBuffers.SceneData->GetNative()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootConstantBufferView(i++, GPUBuffers.Camera->GetNative()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootShaderResourceView(i++, GPUBuffers.InstanceData->GetNative()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootShaderResourceView(i++, GPUBuffers.ObjectData->GetNative()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootDescriptorTable(i++, RenderTextures.Color->GetUAVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(i++, RenderTextures.LinearDepth->GetUAVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(i++, RenderTextures.NormalizedDepth->GetUAVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(i++, RenderTextures.MotionVectors->GetUAVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(i++, RenderTextures.BaseColorMetalness->GetUAVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(i++, RenderTextures.EmissiveColor->GetUAVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(i++, RenderTextures.Normals->GetUAVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(i++, RenderTextures.Roughness->GetUAVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(i++, RenderTextures.NormalRoughness->GetUAVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(i++, RenderTextures.NoisyDiffuse->GetUAVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(i++, RenderTextures.NoisySpecular->GetUAVDescriptor().GPUHandle);

		const D3D12_DISPATCH_RAYS_DESC dispatchRaysDesc{
			.RayGenerationShaderRecord{
				.StartAddress = m_shaderBindingTable->GetRayGenerationAddress(),
				.SizeInBytes = m_shaderBindingTable->GetRayGenerationSize()
			},
			.MissShaderTable{
				.StartAddress = m_shaderBindingTable->GetMissAddress(),
				.SizeInBytes = m_shaderBindingTable->GetMissSize(),
				.StrideInBytes = m_shaderBindingTable->GetMissStride()
			},
			.HitGroupTable{
				.StartAddress = m_shaderBindingTable->GetHitGroupAddress(),
				.SizeInBytes = m_shaderBindingTable->GetHitGroupSize(),
				.StrideInBytes = m_shaderBindingTable->GetHitGroupStride()
			},
			.Width = m_renderSize.x,
			.Height = m_renderSize.y,
			.Depth = 1
		};
		pCommandList->DispatchRays(&dispatchRaysDesc);
	}

private:
	struct ShaderEntryNames { MAKE_NAME(RayGeneration); };

	struct { ConstantBuffer<GraphicsSettings> GraphicsSettings; } m_GPUBuffers;

	ComPtr<ID3D12RootSignature> m_rootSignature;
	ComPtr<ID3D12StateObject> m_pipelineState;

	unique_ptr<ShaderBindingTable> m_shaderBindingTable;

	XMUINT2 m_renderSize{};
};
