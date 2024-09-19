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

import CommandList;
import DeviceContext;
import ErrorHelpers;
import GPUBuffer;
import NRD;
import RTXDIResources;
import Texture;

using namespace DirectX;
using namespace ErrorHelpers;
using namespace Microsoft::WRL;
using namespace rtxdi;
using namespace std;

export struct RTXDI {
	struct { GPUBuffer* Camera, * InstanceData, * ObjectData; } GPUBuffers{};

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
	} Textures{};

	explicit RTXDI(const DeviceContext& deviceContext) noexcept(false) : m_GPUBuffers{
		.GraphicsSettings = GPUBuffer::CreateConstant<GraphicsSettings, false>(deviceContext)
	} {
		{
			constexpr D3D12_SHADER_BYTECODE ShaderByteCode{ g_LocalLightPresampling_dxil, size(g_LocalLightPresampling_dxil) };
			ThrowIfFailed(deviceContext.Device->CreateRootSignature(0, ShaderByteCode.pShaderBytecode, ShaderByteCode.BytecodeLength, IID_PPV_ARGS(&m_rootSignature)));
		}

		const auto CreatePipelineState = [&](ComPtr<ID3D12PipelineState>& pipelineState, auto name, span<const uint8_t> shaderByteCode) {
			const D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS{ data(shaderByteCode), size(shaderByteCode) } };
			ThrowIfFailed(deviceContext.Device->CreateComputePipelineState(&pipelineStateDesc, IID_PPV_ARGS(&pipelineState)));
			pipelineState->SetName(name);
		};
		CreatePipelineState(m_localLightPresampling, L"LocalLightPresampling", g_LocalLightPresampling_dxil);
		CreatePipelineState(m_ReGIRPresampling, L"ReGIRPresampling", g_ReGIRPresampling_dxil);
		CreatePipelineState(m_DIInitialSampling, L"DIInitialSampling", g_DIInitialSampling_dxil);
		CreatePipelineState(m_DITemporalResampling, L"DITemporalResampling", g_DITemporalResampling_dxil);
		CreatePipelineState(m_DISpatialResampling, L"DISpatialResampling", g_DISpatialResampling_dxil);
		CreatePipelineState(m_DIFinalShading, L"DIFinalShading", g_DIFinalShading_dxil);
	}

	void SetConstants(const RTXDIResources& resources, bool isReGIRCellVisualizationEnabled, const NRDSettings& NRDSettings) noexcept {
		m_resources = &resources;

		const auto& context = *m_resources->Context;

		ReGIR_Parameters ReGIRParameters;
		{
			const auto& ReGIRContext = context.getReGIRContext();
			const auto staticParameters = ReGIRContext.getReGIRStaticParameters();
			const auto dynamicParameters = ReGIRContext.getReGIRDynamicParameters();
			const auto onionParameters = ReGIRContext.getReGIROnionCalculatedParameters();
			ReGIRParameters = {
				.commonParams{
					.localLightSamplingFallbackMode = static_cast<uint32_t>(dynamicParameters.fallbackSamplingMode),
					.centerX = dynamicParameters.center.x,
					.centerY = dynamicParameters.center.y,
					.centerZ = dynamicParameters.center.z,
					.risBufferOffset = ReGIRContext.getReGIRCellOffset(),
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
			ranges::copy(onionParameters.regirOnionRings, ReGIRParameters.onionParams.rings);
		}

		const auto& ReSTIRDIContext = context.getReSTIRDIContext();
		const auto& ReSTIRDIStaticParameters = ReSTIRDIContext.getStaticParameters();
		m_graphicsSettings = {
			.RenderSize{ ReSTIRDIStaticParameters.RenderWidth, ReSTIRDIStaticParameters.RenderHeight },
			.IsReGIRCellVisualizationEnabled = isReGIRCellVisualizationEnabled,
			.RTXDI{
				.LocalLightRISBufferSegment = context.getLocalLightRISBufferSegmentParams(),
				.EnvironmentLightRISBufferSegment = context.getEnvironmentLightRISBufferSegmentParams(),
				.LightBuffer = context.getLightBufferParameters(),
				.Runtime = context.getReSTIRDIContext().getRuntimeParams(),
				.ReGIR = ReGIRParameters,
				.ReSTIRDI{
					.reservoirBufferParams = ReSTIRDIContext.getReservoirBufferParameters(),
					.bufferIndices = ReSTIRDIContext.getBufferIndices(),
					.initialSamplingParams = ReSTIRDIContext.getInitialSamplingParameters(),
					.temporalResamplingParams = ReSTIRDIContext.getTemporalResamplingParameters(),
					.spatialResamplingParams = ReSTIRDIContext.getSpatialResamplingParameters(),
					.shadingParams = ReSTIRDIContext.getShadingParameters()
				}
			},
			.NRD = NRDSettings
		};
	}

	void Render(CommandList& commandList, D3D12_GPU_VIRTUAL_ADDRESS topLevelAccelerationStructure) {
		commandList->SetComputeRootSignature(m_rootSignature.Get());

		commandList.Copy(*m_GPUBuffers.GraphicsSettings, initializer_list{ m_graphicsSettings });
		commandList.SetState(*m_GPUBuffers.GraphicsSettings, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);

		commandList.SetState(*GPUBuffers.Camera, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
		commandList.SetState(*GPUBuffers.InstanceData, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		commandList.SetState(*GPUBuffers.ObjectData, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		commandList.SetState(*m_resources->LightInfo, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		commandList.SetState(*m_resources->LightIndices, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		commandList.SetState(*m_resources->LocalLightPDF, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		commandList.SetState(*Textures.PreviousLinearDepth, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		commandList.SetState(*Textures.LinearDepth, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		commandList.SetState(*Textures.MotionVectors, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		commandList.SetState(*Textures.PreviousBaseColorMetalness, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		commandList.SetState(*Textures.BaseColorMetalness, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		commandList.SetState(*Textures.PreviousNormals, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		commandList.SetState(*Textures.Normals, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		commandList.SetState(*Textures.PreviousRoughness, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		commandList.SetState(*Textures.Roughness, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		commandList.SetState(*m_resources->RIS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		commandList.SetState(*m_resources->RISLightInfo, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		commandList.SetState(*m_resources->DIReservoir, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		commandList.SetState(*Textures.Color, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		commandList.SetState(*Textures.NoisyDiffuse, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		commandList.SetState(*Textures.NoisySpecular, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		UINT i = 0;
		commandList->SetComputeRootShaderResourceView(i++, topLevelAccelerationStructure);
		commandList->SetComputeRootConstantBufferView(i++, m_GPUBuffers.GraphicsSettings->GetNative()->GetGPUVirtualAddress());
		commandList->SetComputeRootConstantBufferView(i++, GPUBuffers.Camera->GetNative()->GetGPUVirtualAddress());
		commandList->SetComputeRootShaderResourceView(i++, GPUBuffers.InstanceData->GetNative()->GetGPUVirtualAddress());
		commandList->SetComputeRootShaderResourceView(i++, GPUBuffers.ObjectData->GetNative()->GetGPUVirtualAddress());
		commandList->SetComputeRootShaderResourceView(i++, m_resources->LightInfo->GetNative()->GetGPUVirtualAddress());
		commandList->SetComputeRootShaderResourceView(i++, m_resources->LightIndices->GetNative()->GetGPUVirtualAddress());
		commandList->SetComputeRootDescriptorTable(i++, m_resources->NeighborOffsets->GetSRVDescriptor(BufferSRVType::Typed));
		commandList->SetComputeRootDescriptorTable(i++, m_resources->LocalLightPDF->GetSRVDescriptor());
		commandList->SetComputeRootDescriptorTable(i++, Textures.PreviousLinearDepth->GetSRVDescriptor());
		commandList->SetComputeRootDescriptorTable(i++, Textures.LinearDepth->GetSRVDescriptor());
		commandList->SetComputeRootDescriptorTable(i++, Textures.MotionVectors->GetSRVDescriptor());
		commandList->SetComputeRootDescriptorTable(i++, Textures.PreviousBaseColorMetalness->GetSRVDescriptor());
		commandList->SetComputeRootDescriptorTable(i++, Textures.BaseColorMetalness->GetSRVDescriptor());
		commandList->SetComputeRootDescriptorTable(i++, Textures.PreviousNormals->GetSRVDescriptor());
		commandList->SetComputeRootDescriptorTable(i++, Textures.Normals->GetSRVDescriptor());
		commandList->SetComputeRootDescriptorTable(i++, Textures.PreviousRoughness->GetSRVDescriptor());
		commandList->SetComputeRootDescriptorTable(i++, Textures.Roughness->GetSRVDescriptor());
		commandList->SetComputeRootUnorderedAccessView(i++, m_resources->RIS->GetNative()->GetGPUVirtualAddress());
		commandList->SetComputeRootUnorderedAccessView(i++, m_resources->RISLightInfo->GetNative()->GetGPUVirtualAddress());
		commandList->SetComputeRootUnorderedAccessView(i++, m_resources->DIReservoir->GetNative()->GetGPUVirtualAddress());
		commandList->SetComputeRootDescriptorTable(i++, Textures.Color->GetUAVDescriptor());
		commandList->SetComputeRootDescriptorTable(i++, Textures.NoisyDiffuse->GetUAVDescriptor());
		commandList->SetComputeRootDescriptorTable(i++, Textures.NoisySpecular->GetUAVDescriptor());

		const auto& context = *m_resources->Context;

		if (context.getLightBufferParameters().localLightBufferRegion.numLights) {
			const auto Dispatch = [&](const ComPtr<ID3D12PipelineState>& pipelineState, XMUINT2 size) {
				commandList->SetPipelineState(pipelineState.Get());

				commandList->Dispatch((size.x + 255) / 256, size.y, 1);

				commandList.SetUAVBarrier(*m_resources->RIS);
			};

			if (context.isLocalLightPowerRISEnabled()) {
				const auto localLightRISBufferSegment = context.getLocalLightRISBufferSegmentParams();
				Dispatch(m_localLightPresampling, { localLightRISBufferSegment.tileSize, localLightRISBufferSegment.tileCount });
			}

			if (context.isReGIREnabled()) {
				Dispatch(m_ReGIRPresampling, { context.getReGIRContext().getReGIRLightSlotCount(), 1 });
			}
		}

		const auto Dispatch = [&](const ComPtr<ID3D12PipelineState>& pipelineState) {
			commandList->SetPipelineState(pipelineState.Get());

			const auto& parameters = context.getReSTIRDIContext().getStaticParameters();
			commandList->Dispatch((parameters.RenderWidth + 7) / 8, (parameters.RenderHeight + 7) / 8, 1);

			commandList.SetUAVBarrier(*m_resources->DIReservoir);
		};

		Dispatch(m_DIInitialSampling);
		Dispatch(m_DITemporalResampling);
		Dispatch(m_DISpatialResampling);
		Dispatch(m_DIFinalShading);
	}

private:
	struct alignas(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT) GraphicsSettings {
		XMUINT2 RenderSize;
		BOOL IsReGIRCellVisualizationEnabled;
		UINT _;
		struct {
			RTXDI_RISBufferSegmentParameters LocalLightRISBufferSegment, EnvironmentLightRISBufferSegment;
			RTXDI_LightBufferParameters LightBuffer;
			RTXDI_RuntimeParameters Runtime;
			ReGIR_Parameters ReGIR;
			ReSTIRDI_Parameters ReSTIRDI;
		} RTXDI;
		NRDSettings NRD;
	};

	struct { unique_ptr<GPUBuffer> GraphicsSettings; } m_GPUBuffers;

	ComPtr<ID3D12RootSignature> m_rootSignature;
	ComPtr<ID3D12PipelineState>
		m_localLightPresampling, m_ReGIRPresampling,
		m_DIInitialSampling, m_DITemporalResampling, m_DISpatialResampling, m_DIFinalShading;

	const RTXDIResources* m_resources{};

	GraphicsSettings m_graphicsSettings;
};
