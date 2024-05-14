module;

#include <DirectXMath.h>

#include "directx/d3dx12.h"

#include "directxtk12/DirectXHelpers.h"

#include "Shaders/Raytracing.dxil.h"

export module Raytracing;

import Camera;
import CommonShaderData;
import ErrorHelpers;
import GPUBuffer;
import RaytracingHelpers;
import Scene;
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
		UINT FrameIndex, Bounces, SamplesPerPixel;
		BOOL IsRussianRouletteEnabled, IsShaderExecutionReorderingEnabled;
		UINT _;
		NRDSettings NRD;
	};

	struct {
		ConstantBuffer<SceneData>* InSceneData;
		ConstantBuffer<Camera>* InCamera;
		UploadBuffer<InstanceData>* InInstanceData;
		UploadBuffer<ObjectData>* InObjectData;
	} GPUBuffers{};

	struct {
		Texture
			* OutColor,
			* OutLinearDepth,
			* OutNormalizedDepth,
			* OutMotionVectors,
			* OutBaseColorMetalness,
			* OutEmissiveColor,
			* OutNormals,
			* OutRoughness,
			* OutNormalRoughness,
			* OutNoisyDiffuse,
			* OutNoisySpecular;
	} RenderTextures{};

	explicit Raytracing(ID3D12Device5* pDevice) noexcept(false) : m_device(pDevice), m_GPUBuffers{ .GraphicsSettings = ConstantBuffer<GraphicsSettings>(pDevice) } {
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
	}

	void SetConstants(const GraphicsSettings& graphicsSettings) noexcept {
		m_GPUBuffers.GraphicsSettings.At(0) = graphicsSettings;
		m_renderSize = graphicsSettings.RenderSize;
	}

	void SetScene(const Scene* pScene) {
		m_scene = pScene;

		vector<ShaderBindingTable::Entry>
			rayGenerationEntries{ { ShaderEntryNames::RayGeneration } },
			missEntries,
			hitGroupEntries;
		ComPtr<ID3D12StateObjectProperties> stateObjectProperties;
		ThrowIfFailed(m_pipelineState->QueryInterface(IID_PPV_ARGS(&stateObjectProperties)));
		m_shaderBindingTable = make_unique<ShaderBindingTable>(m_device, stateObjectProperties.Get(), rayGenerationEntries, missEntries, hitGroupEntries);
	}

	void Render(ID3D12GraphicsCommandList4* pCommandList) {
		const ScopedBarrier scopedBarrier(
			pCommandList,
			{
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.OutColor, RenderTextures.OutColor->GetState(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.OutLinearDepth, RenderTextures.OutLinearDepth->GetState(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.OutNormalizedDepth, RenderTextures.OutNormalizedDepth->GetState(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.OutMotionVectors, RenderTextures.OutMotionVectors->GetState(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.OutBaseColorMetalness, RenderTextures.OutBaseColorMetalness->GetState(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.OutEmissiveColor, RenderTextures.OutEmissiveColor->GetState(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.OutNormals, RenderTextures.OutNormals->GetState(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.OutRoughness, RenderTextures.OutRoughness->GetState(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.OutNormalRoughness, RenderTextures.OutNormalRoughness->GetState(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.OutNoisyDiffuse, RenderTextures.OutNoisyDiffuse->GetState(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.OutNoisySpecular, RenderTextures.OutNoisySpecular->GetState(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
			}
		);

		pCommandList->SetPipelineState1(m_pipelineState.Get());
		pCommandList->SetComputeRootSignature(m_rootSignature.Get());
		pCommandList->SetComputeRootShaderResourceView(0, m_scene->GetTopLevelAccelerationStructure().GetBuffer()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootConstantBufferView(1, m_GPUBuffers.GraphicsSettings.GetNative()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootConstantBufferView(2, GPUBuffers.InSceneData->GetNative()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootConstantBufferView(3, GPUBuffers.InCamera->GetNative()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootShaderResourceView(4, GPUBuffers.InInstanceData->GetNative()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootShaderResourceView(5, GPUBuffers.InObjectData->GetNative()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootDescriptorTable(6, RenderTextures.OutColor->GetUAVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(7, RenderTextures.OutLinearDepth->GetUAVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(8, RenderTextures.OutNormalizedDepth->GetUAVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(9, RenderTextures.OutMotionVectors->GetUAVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(10, RenderTextures.OutBaseColorMetalness->GetUAVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(11, RenderTextures.OutEmissiveColor->GetUAVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(12, RenderTextures.OutNormals->GetUAVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(13, RenderTextures.OutRoughness->GetUAVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(14, RenderTextures.OutNormalRoughness->GetUAVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(15, RenderTextures.OutNoisyDiffuse->GetUAVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(16, RenderTextures.OutNoisySpecular->GetUAVDescriptor().GPUHandle);

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

	ID3D12Device5* m_device;

	struct { ConstantBuffer<GraphicsSettings> GraphicsSettings; } m_GPUBuffers;

	ComPtr<ID3D12RootSignature> m_rootSignature;
	ComPtr<ID3D12StateObject> m_pipelineState;

	unique_ptr<ShaderBindingTable> m_shaderBindingTable;

	XMUINT2 m_renderSize{};

	const Scene* m_scene{};
};
