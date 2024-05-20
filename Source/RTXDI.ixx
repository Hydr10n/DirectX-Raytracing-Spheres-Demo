module;

#include "directx/d3dx12.h"

#include "directxtk12/DirectXHelpers.h"

#include "rtxdi/ReSTIRDIParameters.h"

#include "Shaders/DIFinalShading.dxil.h"
#include "Shaders/DIFusedSpatioTemporalResampling.dxil.h"
#include "Shaders/DIInitialSamplesGeneration.dxil.h"

export module RTXDI;

import Camera;
import CommonShaderData;
import ErrorHelpers;
import GPUBuffer;
import NRD;
import RaytracingHelpers;
import RTXDIResources;
import Texture;

using namespace DirectX;
using namespace DirectX::RaytracingHelpers;
using namespace ErrorHelpers;
using namespace Microsoft::WRL;
using namespace std;

export struct RTXDI {
	struct GraphicsSettings {
		XMUINT2 RenderSize;
		UINT FrameIndex, _;
		struct {
			RTXDI_LightBufferParameters LightBuffer;
			RTXDI_RuntimeParameters Runtime;
			ReSTIRDI_Parameters ReSTIRDI;
		} RTXDI;
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

	explicit RTXDI(ID3D12Device* pDevice) noexcept(false) : m_GPUBuffers{ .GraphicsSettings = ConstantBuffer<GraphicsSettings>(pDevice) } {
		constexpr D3D12_SHADER_BYTECODE DIInitialSamplesGenerationShaderByteCode{ g_DIInitialSamplesGeneration_dxil, size(g_DIInitialSamplesGeneration_dxil) };

		ThrowIfFailed(pDevice->CreateRootSignature(0, DIInitialSamplesGenerationShaderByteCode.pShaderBytecode, DIInitialSamplesGenerationShaderByteCode.BytecodeLength, IID_PPV_ARGS(&m_rootSignature)));

		{
			const D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS = DIInitialSamplesGenerationShaderByteCode };
			ThrowIfFailed(pDevice->CreateComputePipelineState(&pipelineStateDesc, IID_PPV_ARGS(&m_DIInitialSamplesGeneration)));
			m_DIInitialSamplesGeneration->SetName(L"DIInitialSamplesGeneration");
		}

		{
			constexpr D3D12_SHADER_BYTECODE ShaderByteCode{ g_DIFusedSpatioTemporalResampling_dxil, size(g_DIFusedSpatioTemporalResampling_dxil) };
			const D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS = ShaderByteCode };
			ThrowIfFailed(pDevice->CreateComputePipelineState(&pipelineStateDesc, IID_PPV_ARGS(&m_DIFusedSpatioTemporalResampling)));
			m_DIFusedSpatioTemporalResampling->SetName(L"DIFusedSpatioTemporalResampling");
		}

		{
			constexpr D3D12_SHADER_BYTECODE ShaderByteCode{ g_DIFinalShading_dxil, size(g_DIFinalShading_dxil) };
			const D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS = ShaderByteCode };
			ThrowIfFailed(pDevice->CreateComputePipelineState(&pipelineStateDesc, IID_PPV_ARGS(&m_DIFinalShading)));
			m_DIFinalShading->SetName(L"DIFinalShading");
		}
	}

	void SetConstants(const GraphicsSettings& graphicsSettings) noexcept {
		m_GPUBuffers.GraphicsSettings.At(0) = graphicsSettings;
		m_renderSize = graphicsSettings.RenderSize;
	}

	void Render(ID3D12GraphicsCommandList4* pCommandList, const TopLevelAccelerationStructure& scene) {
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

		pCommandList->SetComputeRootSignature(m_rootSignature.Get());

		UINT i = 0;
		pCommandList->SetComputeRootShaderResourceView(i++, scene.GetBuffer()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootConstantBufferView(i++, m_GPUBuffers.GraphicsSettings.GetNative()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootConstantBufferView(i++, GPUBuffers.InCamera->GetNative()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootShaderResourceView(i++, GPUBuffers.InInstanceData->GetNative()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootShaderResourceView(i++, GPUBuffers.InObjectData->GetNative()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootShaderResourceView(i++, GPUBuffers.InLightInfo->GetNative()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootShaderResourceView(i++, GPUBuffers.InLightIndices->GetNative()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootDescriptorTable(i++, GPUBuffers.InNeighborOffsets->GetTypedSRVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(i++, RenderTextures.InPreviousLinearDepth->GetSRVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(i++, RenderTextures.InLinearDepth->GetSRVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(i++, RenderTextures.InMotionVectors->GetSRVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(i++, RenderTextures.InPreviousBaseColorMetalness->GetSRVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(i++, RenderTextures.InBaseColorMetalness->GetSRVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(i++, RenderTextures.InPreviousNormals->GetSRVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(i++, RenderTextures.InNormals->GetSRVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(i++, RenderTextures.InPreviousRoughness->GetSRVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(i++, RenderTextures.InRoughness->GetSRVDescriptor().GPUHandle);
		pCommandList->SetComputeRootUnorderedAccessView(i++, GPUBuffers.OutDIReservoir->GetNative()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootDescriptorTable(i++, RenderTextures.OutColor->GetUAVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(i++, RenderTextures.OutNoisyDiffuse->GetUAVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(i++, RenderTextures.OutNoisySpecular->GetUAVDescriptor().GPUHandle);

		const auto Dispatch = [&](const ComPtr<ID3D12PipelineState>& pipelineState) {
			const auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(*GPUBuffers.OutDIReservoir);
			pCommandList->ResourceBarrier(1, &barrier);

			pCommandList->SetPipelineState(pipelineState.Get());
			pCommandList->Dispatch((m_renderSize.x + 7) / 8, (m_renderSize.y + 7) / 8, 1);
		};
		Dispatch(m_DIInitialSamplesGeneration);
		Dispatch(m_DIFusedSpatioTemporalResampling);
		Dispatch(m_DIFinalShading);
	}

private:
	struct { ConstantBuffer<GraphicsSettings> GraphicsSettings; } m_GPUBuffers;

	ComPtr<ID3D12RootSignature> m_rootSignature;
	ComPtr<ID3D12PipelineState> m_DIInitialSamplesGeneration, m_DIFusedSpatioTemporalResampling, m_DIFinalShading;

	XMUINT2 m_renderSize{};
};
