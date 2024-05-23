module;

#include "directx/d3dx12.h"

#include "directxtk12/DirectXHelpers.h"

#include "rtxdi/ReSTIRDIParameters.h"

#include "Shaders/DIFinalShading.dxil.h"
#include "Shaders/DIInitialSampling.dxil.h"
#include "Shaders/DISpatialResampling.dxil.h"
#include "Shaders/DITemporalResampling.dxil.h"

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
		ConstantBuffer<Camera>* Camera;
		UploadBuffer<InstanceData>* InstanceData;
		UploadBuffer<ObjectData>* ObjectData;
		DefaultBuffer<LightInfo>* LightInfo;
		DefaultBuffer<UINT>* LightIndices;
		DefaultBuffer<UINT16>* NeighborOffsets;
		DefaultBuffer<RTXDI_PackedDIReservoir>* DIReservoir;
	} GPUBuffers{};

	struct {
		Texture
			* PreviousLinearDepth,
			* LinearDepth,
			* MotionVectors,
			* PreviousBaseColorMetalness,
			* BaseColorMetalness,
			* PreviousNormals,
			* Normals,
			* PreviousRoughness,
			* Roughness,
			* Color,
			* NoisyDiffuse,
			* NoisySpecular;
	} RenderTextures{};

	explicit RTXDI(ID3D12Device* pDevice) noexcept(false) : m_GPUBuffers{ .GraphicsSettings = ConstantBuffer<GraphicsSettings>(pDevice) } {
		constexpr D3D12_SHADER_BYTECODE DIInitialSamplingShaderByteCode{ g_DIInitialSampling_dxil, size(g_DIInitialSampling_dxil) };

		ThrowIfFailed(pDevice->CreateRootSignature(0, DIInitialSamplingShaderByteCode.pShaderBytecode, DIInitialSamplingShaderByteCode.BytecodeLength, IID_PPV_ARGS(&m_rootSignature)));

		{
			const D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS = DIInitialSamplingShaderByteCode };
			ThrowIfFailed(pDevice->CreateComputePipelineState(&pipelineStateDesc, IID_PPV_ARGS(&m_DIInitialSampling)));
			m_DIInitialSampling->SetName(L"DIInitialSampling");
		}

		{
			constexpr D3D12_SHADER_BYTECODE ShaderByteCode{ g_DITemporalResampling_dxil, size(g_DITemporalResampling_dxil) };
			const D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS = ShaderByteCode };
			ThrowIfFailed(pDevice->CreateComputePipelineState(&pipelineStateDesc, IID_PPV_ARGS(&m_DITemporalResampling)));
			m_DITemporalResampling->SetName(L"DITemporalResampling");
		}

		{
			constexpr D3D12_SHADER_BYTECODE ShaderByteCode{ g_DISpatialResampling_dxil, size(g_DISpatialResampling_dxil) };
			const D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS = ShaderByteCode };
			ThrowIfFailed(pDevice->CreateComputePipelineState(&pipelineStateDesc, IID_PPV_ARGS(&m_DISpatialResampling)));
			m_DISpatialResampling->SetName(L"DISpatialResampling");
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
				CD3DX12_RESOURCE_BARRIER::Transition(*GPUBuffers.LightInfo, GPUBuffers.LightInfo->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.PreviousLinearDepth, RenderTextures.PreviousLinearDepth->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.LinearDepth, RenderTextures.LinearDepth->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.MotionVectors, RenderTextures.MotionVectors->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.PreviousBaseColorMetalness, RenderTextures.PreviousBaseColorMetalness->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.BaseColorMetalness, RenderTextures.BaseColorMetalness->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.PreviousNormals, RenderTextures.PreviousNormals->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.Normals, RenderTextures.Normals->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.PreviousRoughness, RenderTextures.PreviousRoughness->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.Roughness, RenderTextures.Roughness->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.Color, RenderTextures.Color->GetState(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.NoisyDiffuse, RenderTextures.NoisyDiffuse->GetState(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.NoisySpecular, RenderTextures.NoisySpecular->GetState(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
			}
		);

		pCommandList->SetComputeRootSignature(m_rootSignature.Get());

		UINT i = 0;
		pCommandList->SetComputeRootShaderResourceView(i++, scene.GetBuffer()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootConstantBufferView(i++, m_GPUBuffers.GraphicsSettings.GetNative()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootConstantBufferView(i++, GPUBuffers.Camera->GetNative()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootShaderResourceView(i++, GPUBuffers.InstanceData->GetNative()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootShaderResourceView(i++, GPUBuffers.ObjectData->GetNative()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootShaderResourceView(i++, GPUBuffers.LightInfo->GetNative()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootShaderResourceView(i++, GPUBuffers.LightIndices->GetNative()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootDescriptorTable(i++, GPUBuffers.NeighborOffsets->GetTypedSRVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(i++, RenderTextures.PreviousLinearDepth->GetSRVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(i++, RenderTextures.LinearDepth->GetSRVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(i++, RenderTextures.MotionVectors->GetSRVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(i++, RenderTextures.PreviousBaseColorMetalness->GetSRVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(i++, RenderTextures.BaseColorMetalness->GetSRVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(i++, RenderTextures.PreviousNormals->GetSRVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(i++, RenderTextures.Normals->GetSRVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(i++, RenderTextures.PreviousRoughness->GetSRVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(i++, RenderTextures.Roughness->GetSRVDescriptor().GPUHandle);
		pCommandList->SetComputeRootUnorderedAccessView(i++, GPUBuffers.DIReservoir->GetNative()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootDescriptorTable(i++, RenderTextures.Color->GetUAVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(i++, RenderTextures.NoisyDiffuse->GetUAVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(i++, RenderTextures.NoisySpecular->GetUAVDescriptor().GPUHandle);

		const auto Dispatch = [&](const ComPtr<ID3D12PipelineState>& pipelineState) {
			const auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(*GPUBuffers.DIReservoir);
			pCommandList->ResourceBarrier(1, &barrier);

			pCommandList->SetPipelineState(pipelineState.Get());

			pCommandList->Dispatch((m_renderSize.x + 7) / 8, (m_renderSize.y + 7) / 8, 1);
		};

		Dispatch(m_DIInitialSampling);
		Dispatch(m_DITemporalResampling);
		Dispatch(m_DISpatialResampling);
		Dispatch(m_DIFinalShading);
	}

private:
	struct { ConstantBuffer<GraphicsSettings> GraphicsSettings; } m_GPUBuffers;

	ComPtr<ID3D12RootSignature> m_rootSignature;
	ComPtr<ID3D12PipelineState> m_DIInitialSampling, m_DITemporalResampling, m_DISpatialResampling, m_DIFinalShading;

	XMUINT2 m_renderSize{};
};
