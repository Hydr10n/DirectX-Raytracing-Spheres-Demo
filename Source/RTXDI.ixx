module;

#include <ranges>

#include "directx/d3dx12.h"

#include "directxtk12/DirectXHelpers.h"

#include "rtxdi/ImportanceSamplingContext.h"

#include "Shaders/DIFinalShading.dxil.h"
#include "Shaders/DIInitialSampling.dxil.h"
#include "Shaders/DISpatialResampling.dxil.h"
#include "Shaders/DITemporalResampling.dxil.h"
#include "Shaders/LocalLightPresampling.dxil.h"
#include "Shaders/ReGIRPresampling.dxil.h"

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
using namespace rtxdi;
using namespace std;

export struct RTXDI {
	struct GraphicsSettings {
		XMUINT2 RenderSize;
		BOOL VisualizeReGIRCells;
		UINT _;
		struct {
			RTXDI_RISBufferSegmentParameters LocalLightRISBufferSegment, EnvironmentLightRISBufferSegment;
			RTXDI_LightBufferParameters LightBuffer;
			RTXDI_RuntimeParameters Runtime;
			ReSTIRDI_Parameters ReSTIRDI;
			ReGIR_Parameters ReGIR;
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
			constexpr D3D12_SHADER_BYTECODE ShaderByteCode{ g_ReGIRPresampling_dxil, size(g_ReGIRPresampling_dxil) };
			const D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS = ShaderByteCode };
			ThrowIfFailed(pDevice->CreateComputePipelineState(&pipelineStateDesc, IID_PPV_ARGS(&m_ReGIRPresampling)));
			m_ReGIRPresampling->SetName(L"ReGIRPresampling");
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

	void SetConstants(const ImportanceSamplingContext& importanceSamplingContext, bool visualizeReGIRCells, const NRDSettings& NRDSettings) {
		m_context = &importanceSamplingContext;

		ReGIR_Parameters ReGIRParameters;
		{
			const auto& context = importanceSamplingContext.getReGIRContext();
			const auto staticParameters = context.getReGIRStaticParameters();
			const auto dynamicParameters = context.getReGIRDynamicParameters();
			const auto onionParameters = context.getReGIROnionCalculatedParameters();
			ReGIRParameters = {
				.commonParams{
					.localLightSamplingFallbackMode = static_cast<uint32_t>(dynamicParameters.fallbackSamplingMode),
					.centerX = dynamicParameters.center.x,
					.centerY = dynamicParameters.center.y,
					.centerZ = dynamicParameters.center.z,
					.risBufferOffset = context.getReGIRCellOffset(),
					.lightsPerCell = staticParameters.LightsPerCell,
					.cellSize = dynamicParameters.regirCellSize * (staticParameters.Mode == ReGIRMode::Onion ? 0.5f : 1),
					.samplingJitter = max(0.0f, dynamicParameters.regirSamplingJitter * 2),
					.localLightPresamplingMode = static_cast<uint32_t>(dynamicParameters.presamplingMode),
					.numRegirBuildSamples = dynamicParameters.regirNumBuildSamples
				},
				.gridParams{
					.cellsX = staticParameters.gridParameters.GridSize.x,
					.cellsY = staticParameters.gridParameters.GridSize.y,
					.cellsZ = staticParameters.gridParameters.GridSize.z
				},
				.onionParams{
					.numLayerGroups = static_cast<uint32_t>(size(onionParameters.regirOnionLayers)),
					.cubicRootFactor = onionParameters.regirOnionCubicRootFactor,
					.linearFactor = onionParameters.regirOnionLinearFactor
				}
			};
			for (const auto i : views::iota(static_cast<size_t>(0), size(onionParameters.regirOnionLayers))) {
				ReGIRParameters.onionParams.layers[i] = onionParameters.regirOnionLayers[i];
				ReGIRParameters.onionParams.layers[i].innerRadius *= ReGIRParameters.commonParams.cellSize;
				ReGIRParameters.onionParams.layers[i].outerRadius *= ReGIRParameters.commonParams.cellSize;
			}
			for (const auto i : views::iota(static_cast<size_t>(0), size(onionParameters.regirOnionRings))) {
				ReGIRParameters.onionParams.rings[i] = onionParameters.regirOnionRings[i];
			}
		}

		auto& ReSTIRDIContext = importanceSamplingContext.getReSTIRDIContext();
		const auto& ReSTIRDIStaticParameters = ReSTIRDIContext.getStaticParameters();

		m_GPUBuffers.GraphicsSettings.At(0) = {
			.RenderSize{ ReSTIRDIStaticParameters.RenderWidth, ReSTIRDIStaticParameters.RenderHeight },
			.VisualizeReGIRCells = visualizeReGIRCells,
			.RTXDI{
				.LocalLightRISBufferSegment = importanceSamplingContext.getLocalLightRISBufferSegmentParams(),
				.EnvironmentLightRISBufferSegment = importanceSamplingContext.getEnvironmentLightRISBufferSegmentParams(),
				.LightBuffer = importanceSamplingContext.getLightBufferParameters(),
				.Runtime = importanceSamplingContext.getReSTIRDIContext().getRuntimeParams(),
				.ReSTIRDI{
					.reservoirBufferParams = ReSTIRDIContext.getReservoirBufferParameters(),
					.bufferIndices = ReSTIRDIContext.getBufferIndices(),
					.initialSamplingParams = ReSTIRDIContext.getInitialSamplingParameters(),
					.temporalResamplingParams = ReSTIRDIContext.getTemporalResamplingParameters(),
					.spatialResamplingParams = ReSTIRDIContext.getSpatialResamplingParameters(),
					.shadingParams = ReSTIRDIContext.getShadingParameters()
				},
				.ReGIR = ReGIRParameters
			},
			.NRD = NRDSettings
		};
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

			if (m_context->getLightBufferParameters().localLightBufferRegion.numLights) {
				if (m_context->isLocalLightPowerRISEnabled()) {
					const auto localLightRISBufferSegment = m_context->getLocalLightRISBufferSegmentParams();
					Dispatch(m_localLightPresampling, { localLightRISBufferSegment.tileSize, localLightRISBufferSegment.tileCount });
				}

				if (m_context->isReGIREnabled()) {
					Dispatch(m_ReGIRPresampling, { m_context->getReGIRContext().getReGIRLightSlotCount(), 1 });
				}
			}
		}

		{
			const auto Dispatch = [&](const ComPtr<ID3D12PipelineState>& pipelineState) {
				GPUBuffers.DIReservoir->InsertUAVBarrier(pCommandList);

				pCommandList->SetPipelineState(pipelineState.Get());

				const auto& parameters = m_context->getReSTIRDIContext().getStaticParameters();
				const XMUINT2 renderSize{ parameters.RenderWidth, parameters.RenderHeight };
				pCommandList->Dispatch((renderSize.x + 7) / 8, (renderSize.y + 7) / 8, 1);
			};

			Dispatch(m_DIInitialSampling);

			const auto resamplingMode = m_context->getReSTIRDIContext().getResamplingMode();
			if (resamplingMode == ReSTIRDI_ResamplingMode::Temporal || resamplingMode == ReSTIRDI_ResamplingMode::TemporalAndSpatial) {
				Dispatch(m_DITemporalResampling);
			}
			if (resamplingMode == ReSTIRDI_ResamplingMode::Spatial || resamplingMode == ReSTIRDI_ResamplingMode::TemporalAndSpatial) {
				Dispatch(m_DISpatialResampling);
			}

			Dispatch(m_DIFinalShading);
		}
	}

private:
	struct { ConstantBuffer<GraphicsSettings> GraphicsSettings; } m_GPUBuffers;

	ComPtr<ID3D12RootSignature> m_rootSignature;
	ComPtr<ID3D12PipelineState>
		m_localLightPresampling, m_ReGIRPresampling,
		m_DIInitialSampling, m_DITemporalResampling, m_DISpatialResampling, m_DIFinalShading;

	const ImportanceSamplingContext* m_context;
};
