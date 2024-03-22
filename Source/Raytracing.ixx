module;

#include <DirectXMath.h>

#include "directx/d3dx12.h"

#include "directxtk12/DirectXHelpers.h"

#include "rtxdi/RtxdiParameters.h"

#include "Shaders/Raytracing.dxil.h"

export module Raytracing;

import Camera;
import CommonShaderData;
import ErrorHelpers;
import GPUBuffer;
import NRD;
import RTXDIResources;
import Scene;
import Texture;

using namespace DirectX;
using namespace ErrorHelpers;
using namespace Microsoft::WRL;
using namespace std;

export struct Raytracing {
	struct RTXDISettings {
		BOOL IsEnabled;
		UINT LocalLightSamples, BRDFSamples, SpatioTemporalSamples, InputBufferIndex, OutputBufferIndex, UniformRandomNumber;
		UINT _;
		RTXDI_LightBufferParameters LightBufferParameters;
		RTXDI_RuntimeParameters RuntimeParameters;
		RTXDI_ReservoirBufferParameters ReservoirBufferParameters;
	};

	struct NRDSettings {
		NRDDenoiser Denoiser;
		XMUINT3 _;
		XMFLOAT4 HitDistanceParameters;
	};

	struct GraphicsSettings {
		XMUINT2 RenderSize;
		UINT FrameIndex, Bounces, SamplesPerPixel;
		BOOL IsRussianRouletteEnabled;
		XMUINT2 _;
		RTXDISettings RTXDI;
		NRDSettings NRD;
	};

	struct {
		ConstantBuffer<SceneData>* InSceneData;
		ConstantBuffer<Camera>* InCamera;
		UploadBuffer<InstanceData>* InInstanceData;
		UploadBuffer<ObjectData>* InObjectData;
		DefaultBuffer<RAB_LightInfo>* InLightInfo;
		DefaultBuffer<UINT>* InLightIndices;
		DefaultBuffer<UINT16>* InNeighborOffsets;
		DefaultBuffer<RTXDI_PackedDIReservoir>* OutDIReservoir;
	} GPUBuffers{};

	struct {
		Texture
			* InPreviousLinearDepth,
			* InPreviousBaseColorMetalness,
			* InPreviousNormalRoughness,
			* InPreviousGeometricNormals,
			* OutColor,
			* OutLinearDepth,
			* OutNormalizedDepth,
			* OutMotionVectors,
			* OutBaseColorMetalness,
			* OutEmissiveColor,
			* OutNormalRoughness,
			* OutGeometricNormals,
			* OutNoisyDiffuse,
			* OutNoisySpecular;
	} RenderTextures{};

	explicit Raytracing(ID3D12Device* pDevice) noexcept(false) : m_GPUBuffers{ .GraphicsSettings = ConstantBuffer<GraphicsSettings>(pDevice) } {
		constexpr D3D12_SHADER_BYTECODE ShaderByteCode{ g_Raytracing_dxil, size(g_Raytracing_dxil) };
		ThrowIfFailed(pDevice->CreateRootSignature(0, ShaderByteCode.pShaderBytecode, ShaderByteCode.BytecodeLength, IID_PPV_ARGS(&m_rootSignature)));
		const D3D12_COMPUTE_PIPELINE_STATE_DESC computePipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS = ShaderByteCode };
		ThrowIfFailed(pDevice->CreateComputePipelineState(&computePipelineStateDesc, IID_PPV_ARGS(&m_pipelineState)));
		m_pipelineState->SetName(L"Raytracing");
	}

	void SetScene(const Scene* pScene) { m_scene = pScene; }

	void SetConstants(const GraphicsSettings& graphicsSettings) noexcept { m_GPUBuffers.GraphicsSettings.At(0) = graphicsSettings; }

	void Render(ID3D12GraphicsCommandList* pCommandList) {
		const ScopedBarrier scopedBarrier(
			pCommandList,
			{
				CD3DX12_RESOURCE_BARRIER::Transition(*GPUBuffers.InLightIndices, GPUBuffers.InLightIndices->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.InPreviousLinearDepth, RenderTextures.InPreviousLinearDepth->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.InPreviousBaseColorMetalness, RenderTextures.InPreviousBaseColorMetalness->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.InPreviousNormalRoughness, RenderTextures.InPreviousNormalRoughness->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.InPreviousGeometricNormals, RenderTextures.InPreviousGeometricNormals->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.OutColor, RenderTextures.OutColor->GetState(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.OutLinearDepth, RenderTextures.OutLinearDepth->GetState(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.OutNormalizedDepth, RenderTextures.OutNormalizedDepth->GetState(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.OutMotionVectors, RenderTextures.OutMotionVectors->GetState(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.OutBaseColorMetalness, RenderTextures.OutBaseColorMetalness->GetState(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.OutEmissiveColor, RenderTextures.OutEmissiveColor->GetState(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.OutNormalRoughness, RenderTextures.OutNormalRoughness->GetState(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.OutGeometricNormals, RenderTextures.OutGeometricNormals->GetState(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.OutNoisyDiffuse, RenderTextures.OutNoisyDiffuse->GetState(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.OutNoisySpecular, RenderTextures.OutNoisySpecular->GetState(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
			}
		);
		pCommandList->SetPipelineState(m_pipelineState.Get());
		pCommandList->SetComputeRootSignature(m_rootSignature.Get());
		pCommandList->SetComputeRootShaderResourceView(0, m_scene->GetTopLevelAccelerationStructure().GetBuffer()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootConstantBufferView(1, m_GPUBuffers.GraphicsSettings.GetNative()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootConstantBufferView(2, GPUBuffers.InSceneData->GetNative()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootConstantBufferView(3, GPUBuffers.InCamera->GetNative()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootShaderResourceView(4, GPUBuffers.InInstanceData->GetNative()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootShaderResourceView(5, GPUBuffers.InObjectData->GetNative()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootDescriptorTable(6, RenderTextures.InPreviousLinearDepth->GetSRVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(7, RenderTextures.InPreviousBaseColorMetalness->GetSRVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(8, RenderTextures.InPreviousNormalRoughness->GetSRVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(9, RenderTextures.InPreviousGeometricNormals->GetSRVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(10, RenderTextures.OutColor->GetUAVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(11, RenderTextures.OutLinearDepth->GetUAVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(12, RenderTextures.OutNormalizedDepth->GetUAVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(13, RenderTextures.OutMotionVectors->GetUAVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(14, RenderTextures.OutBaseColorMetalness->GetUAVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(15, RenderTextures.OutEmissiveColor->GetUAVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(16, RenderTextures.OutNormalRoughness->GetUAVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(17, RenderTextures.OutGeometricNormals->GetUAVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(18, RenderTextures.OutNoisyDiffuse->GetUAVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(19, RenderTextures.OutNoisySpecular->GetUAVDescriptor().GPUHandle);
		pCommandList->SetComputeRootShaderResourceView(20, GPUBuffers.InLightInfo ? GPUBuffers.InLightInfo->GetNative()->GetGPUVirtualAddress() : NULL);
		pCommandList->SetComputeRootShaderResourceView(21, GPUBuffers.InLightIndices ? GPUBuffers.InLightIndices->GetNative()->GetGPUVirtualAddress() : NULL);
		pCommandList->SetComputeRootDescriptorTable(22, GPUBuffers.InNeighborOffsets->GetTypedSRVDescriptor().GPUHandle);
		pCommandList->SetComputeRootUnorderedAccessView(23, GPUBuffers.OutDIReservoir ? GPUBuffers.OutDIReservoir->GetNative()->GetGPUVirtualAddress() : NULL);
		const auto renderSize = m_GPUBuffers.GraphicsSettings.At(0).RenderSize;
		pCommandList->Dispatch((renderSize.x + 15) / 16, (renderSize.y + 15) / 16, 1);
	}

private:
	struct { ConstantBuffer<GraphicsSettings> GraphicsSettings; } m_GPUBuffers;

	ComPtr<ID3D12RootSignature> m_rootSignature;
	ComPtr<ID3D12PipelineState> m_pipelineState;

	const Scene* m_scene{};
};
