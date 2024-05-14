module;

#include <DirectXMath.h>

#include "directx/d3dx12.h"

#include "directxtk12/DirectXHelpers.h"

#include "rtxdi/RtxdiParameters.h"

#include "Shaders/DirectIllumination.dxil.h"

export module DirectIllumination;

import Camera;
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

export struct DirectIllumination {
	struct RTXDISettings {
		UINT LocalLightSamples, BRDFSamples, SpatioTemporalSamples, InputBufferIndex, OutputBufferIndex, UniformRandomNumber;
		XMUINT2 _;
		RTXDI_LightBufferParameters LightBufferParameters;
		RTXDI_RuntimeParameters RuntimeParameters;
		RTXDI_ReservoirBufferParameters ReservoirBufferParameters;
	};

	struct GraphicsSettings {
		XMUINT2 RenderSize;
		UINT FrameIndex, _;
		RTXDISettings RTXDI;
		NRDSettings NRD;
	};

	struct {
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
			* InLinearDepth,
			* InMotionVectors,
			* InPreviousBaseColorMetalness,
			* InBaseColorMetalness,
			* InPreviousNormals,
			* InNormals,
			* InPreviousRoughness,
			* InRoughness,
			* OutColor,
			* OutNoisyDiffuse,
			* OutNoisySpecular;
	} RenderTextures{};

	explicit DirectIllumination(ID3D12Device* pDevice) noexcept(false) : m_GPUBuffers{ .GraphicsSettings = ConstantBuffer<GraphicsSettings>(pDevice) } {
		constexpr D3D12_SHADER_BYTECODE ShaderByteCode{ g_DirectIllumination_dxil, size(g_DirectIllumination_dxil) };
		ThrowIfFailed(pDevice->CreateRootSignature(0, ShaderByteCode.pShaderBytecode, ShaderByteCode.BytecodeLength, IID_PPV_ARGS(&m_rootSignature)));
		const D3D12_COMPUTE_PIPELINE_STATE_DESC computePipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS = ShaderByteCode };
		ThrowIfFailed(pDevice->CreateComputePipelineState(&computePipelineStateDesc, IID_PPV_ARGS(&m_pipelineState)));
		m_pipelineState->SetName(L"DirectIllumination");
	}

	void SetConstants(const GraphicsSettings& graphicsSettings) noexcept {
		m_GPUBuffers.GraphicsSettings.At(0) = graphicsSettings;
		m_renderSize = graphicsSettings.RenderSize;
	}

	void SetScene(const Scene* pScene) { m_scene = pScene; }

	void Render(ID3D12GraphicsCommandList4* pCommandList) {
		const ScopedBarrier scopedBarrier(
			pCommandList,
			{
				CD3DX12_RESOURCE_BARRIER::Transition(*GPUBuffers.InLightInfo, GPUBuffers.InLightInfo->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.InPreviousLinearDepth, RenderTextures.InPreviousLinearDepth->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.InLinearDepth, RenderTextures.InLinearDepth->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.InMotionVectors, RenderTextures.InMotionVectors->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.InPreviousBaseColorMetalness, RenderTextures.InPreviousBaseColorMetalness->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.InBaseColorMetalness, RenderTextures.InBaseColorMetalness->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.InPreviousNormals, RenderTextures.InPreviousNormals->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.InNormals, RenderTextures.InNormals->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.InPreviousRoughness, RenderTextures.InPreviousRoughness->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.InRoughness, RenderTextures.InRoughness->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.OutColor, RenderTextures.OutColor->GetState(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.OutNoisyDiffuse, RenderTextures.OutNoisyDiffuse->GetState(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.OutNoisySpecular, RenderTextures.OutNoisySpecular->GetState(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
			}
		);
		pCommandList->SetPipelineState(m_pipelineState.Get());
		pCommandList->SetComputeRootSignature(m_rootSignature.Get());
		pCommandList->SetComputeRootShaderResourceView(0, m_scene->GetTopLevelAccelerationStructure().GetBuffer()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootConstantBufferView(1, m_GPUBuffers.GraphicsSettings.GetNative()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootConstantBufferView(2, GPUBuffers.InCamera->GetNative()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootShaderResourceView(3, GPUBuffers.InInstanceData->GetNative()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootShaderResourceView(4, GPUBuffers.InObjectData->GetNative()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootShaderResourceView(5, GPUBuffers.InLightInfo->GetNative()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootShaderResourceView(6, GPUBuffers.InLightIndices->GetNative()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootDescriptorTable(7, GPUBuffers.InNeighborOffsets->GetTypedSRVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(8, RenderTextures.InPreviousLinearDepth->GetSRVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(9, RenderTextures.InLinearDepth->GetSRVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(10, RenderTextures.InMotionVectors->GetSRVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(11, RenderTextures.InPreviousBaseColorMetalness->GetSRVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(12, RenderTextures.InBaseColorMetalness->GetSRVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(13, RenderTextures.InPreviousNormals->GetSRVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(14, RenderTextures.InNormals->GetSRVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(15, RenderTextures.InPreviousRoughness->GetSRVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(16, RenderTextures.InRoughness->GetSRVDescriptor().GPUHandle);
		pCommandList->SetComputeRootUnorderedAccessView(17, GPUBuffers.OutDIReservoir->GetNative()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootDescriptorTable(18, RenderTextures.OutColor->GetUAVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(19, RenderTextures.OutNoisyDiffuse->GetUAVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(20, RenderTextures.OutNoisySpecular->GetUAVDescriptor().GPUHandle);
		pCommandList->Dispatch((m_renderSize.x + 15) / 16, (m_renderSize.y + 15) / 16, 1);
	}

private:
	struct { ConstantBuffer<GraphicsSettings> GraphicsSettings; } m_GPUBuffers;

	ComPtr<ID3D12RootSignature> m_rootSignature;
	ComPtr<ID3D12PipelineState> m_pipelineState;

	XMUINT2 m_renderSize{};

	const Scene* m_scene{};
};
