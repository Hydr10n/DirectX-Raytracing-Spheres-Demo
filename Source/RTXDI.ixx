module;

#include "directx/d3dx12.h"

#include "directxtk12/DirectXHelpers.h"

#include "rtxdi/ReSTIRDIParameters.h"

#include "Shaders/DIFinalShading.dxil.h"
#include "Shaders/DIInitialSampling.dxil.h"
#include "Shaders/DISpatialResampling.dxil.h"
#include "Shaders/DITemporalResampling.dxil.h"
#include "Shaders/LocalLightPresampling.dxil.h"

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
		XMUINT2 RenderSize, _;
		struct {
			RTXDI_RISBufferSegmentParameters LocalLightRISBufferSegment, EnvironmentLightRISBufferSegment;
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
		DefaultBuffer<XMUINT2>* RIS;
		DefaultBuffer<XMUINT4>* RISLightInfo;
		DefaultBuffer<RTXDI_PackedDIReservoir>* DIReservoir;
	} GPUBuffers{};

	struct {
		Texture
			* LocalLightPDF,
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
	} Textures{};

	explicit RTXDI(ID3D12Device* pDevice) noexcept(false) : m_GPUBuffers{ .GraphicsSettings = ConstantBuffer<GraphicsSettings>(pDevice) } {
		constexpr D3D12_SHADER_BYTECODE DIInitialSamplingShaderByteCode{ g_DIInitialSampling_dxil, size(g_DIInitialSampling_dxil) };

		ThrowIfFailed(pDevice->CreateRootSignature(0, DIInitialSamplingShaderByteCode.pShaderBytecode, DIInitialSamplingShaderByteCode.BytecodeLength, IID_PPV_ARGS(&m_rootSignature)));

		{
			constexpr D3D12_SHADER_BYTECODE ShaderByteCode{ g_LocalLightPresampling_dxil, size(g_LocalLightPresampling_dxil) };
			const D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS = ShaderByteCode };
			ThrowIfFailed(pDevice->CreateComputePipelineState(&pipelineStateDesc, IID_PPV_ARGS(&m_localLightPresampling)));
			m_localLightPresampling->SetName(L"LocalLightPresampling");
		}

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
		m_localLightRISBufferSegment = graphicsSettings.RTXDI.LocalLightRISBufferSegment;
	}

	void Render(ID3D12GraphicsCommandList4* pCommandList, const TopLevelAccelerationStructure& scene) {
		const ScopedBarrier scopedBarrier(
			pCommandList,
			{
				CD3DX12_RESOURCE_BARRIER::Transition(*GPUBuffers.LightInfo, GPUBuffers.LightInfo->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(*Textures.LocalLightPDF, Textures.LocalLightPDF->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(*Textures.PreviousLinearDepth, Textures.PreviousLinearDepth->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(*Textures.LinearDepth, Textures.LinearDepth->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(*Textures.MotionVectors, Textures.MotionVectors->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(*Textures.PreviousBaseColorMetalness, Textures.PreviousBaseColorMetalness->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(*Textures.BaseColorMetalness, Textures.BaseColorMetalness->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(*Textures.PreviousNormals, Textures.PreviousNormals->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(*Textures.Normals, Textures.Normals->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(*Textures.PreviousRoughness, Textures.PreviousRoughness->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(*Textures.Roughness, Textures.Roughness->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(*Textures.Color, Textures.Color->GetState(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				CD3DX12_RESOURCE_BARRIER::Transition(*Textures.NoisyDiffuse, Textures.NoisyDiffuse->GetState(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				CD3DX12_RESOURCE_BARRIER::Transition(*Textures.NoisySpecular, Textures.NoisySpecular->GetState(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
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
		pCommandList->SetComputeRootDescriptorTable(i++, Textures.LocalLightPDF->GetSRVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(i++, Textures.PreviousLinearDepth->GetSRVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(i++, Textures.LinearDepth->GetSRVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(i++, Textures.MotionVectors->GetSRVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(i++, Textures.PreviousBaseColorMetalness->GetSRVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(i++, Textures.BaseColorMetalness->GetSRVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(i++, Textures.PreviousNormals->GetSRVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(i++, Textures.Normals->GetSRVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(i++, Textures.PreviousRoughness->GetSRVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(i++, Textures.Roughness->GetSRVDescriptor().GPUHandle);
		pCommandList->SetComputeRootUnorderedAccessView(i++, GPUBuffers.RIS->GetNative()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootUnorderedAccessView(i++, GPUBuffers.RISLightInfo->GetNative()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootUnorderedAccessView(i++, GPUBuffers.DIReservoir->GetNative()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootDescriptorTable(i++, Textures.Color->GetUAVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(i++, Textures.NoisyDiffuse->GetUAVDescriptor().GPUHandle);
		pCommandList->SetComputeRootDescriptorTable(i++, Textures.NoisySpecular->GetUAVDescriptor().GPUHandle);

		{
			const auto Dispatch = [&](const ComPtr<ID3D12PipelineState>& pipelineState, XMUINT2 size) {
				GPUBuffers.RIS->InsertUAVBarrier(pCommandList);

				pCommandList->SetPipelineState(pipelineState.Get());

				pCommandList->Dispatch((size.x + 255) / 256, size.y, 1);
			};

			Dispatch(m_localLightPresampling, { m_localLightRISBufferSegment.tileSize, m_localLightRISBufferSegment.tileCount });
		}

		{
			const auto Dispatch = [&](const ComPtr<ID3D12PipelineState>& pipelineState) {
				GPUBuffers.DIReservoir->InsertUAVBarrier(pCommandList);

				pCommandList->SetPipelineState(pipelineState.Get());

				pCommandList->Dispatch((m_renderSize.x + 7) / 8, (m_renderSize.y + 7) / 8, 1);
			};

			Dispatch(m_DIInitialSampling);
			Dispatch(m_DITemporalResampling);
			Dispatch(m_DISpatialResampling);
			Dispatch(m_DIFinalShading);
		}
	}

private:
	struct { ConstantBuffer<GraphicsSettings> GraphicsSettings; } m_GPUBuffers;

	ComPtr<ID3D12RootSignature> m_rootSignature;
	ComPtr<ID3D12PipelineState>
		m_localLightPresampling,
		m_DIInitialSampling, m_DITemporalResampling, m_DISpatialResampling, m_DIFinalShading;

	XMUINT2 m_renderSize{};
	RTXDI_RISBufferSegmentParameters m_localLightRISBufferSegment{};
};
