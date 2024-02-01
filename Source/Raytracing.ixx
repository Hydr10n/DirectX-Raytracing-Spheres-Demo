module;

#include "directx/d3dx12.h"

#include <DirectXMath.h>

#include "rtxdi/RtxdiParameters.h"

#include "Shaders/Raytracing.dxil.h"

export module Raytracing;

import Camera;
import CommonShaderData;
import ErrorHelpers;
import GPUBuffer;
import NRD;
import RaytracingHelpers;
import RTXDIResources;

using namespace DirectX;
using namespace DirectX::RaytracingHelpers;
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
		UINT FrameIndex, MaxNumberOfBounces, SamplesPerPixel;
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
		DefaultBuffer<RTXDI_PackedDIReservoir>* OutDIReservoir;
	} GPUBuffers{};

	struct {
		D3D12_GPU_DESCRIPTOR_HANDLE
			InPreviousLinearDepth,
			InPreviousBaseColorMetalness,
			InPreviousNormalRoughness,
			InPreviousGeometricNormals,
			OutColor,
			OutLinearDepth, OutNormalizedDepth,
			OutMotionVectors,
			OutBaseColorMetalness,
			OutEmissiveColor,
			OutNormalRoughness,
			OutGeometricNormals,
			OutNoisyDiffuse, OutNoisySpecular,
			InNeighborOffsets;
	} GPUDescriptors{};

	explicit Raytracing(ID3D12Device* pDevice) noexcept(false) : m_GPUBuffers{ .GraphicsSettings = ConstantBuffer<GraphicsSettings>(pDevice) } {
		constexpr D3D12_SHADER_BYTECODE ShaderByteCode{ g_Raytracing_dxil, size(g_Raytracing_dxil) };
		ThrowIfFailed(pDevice->CreateRootSignature(0, ShaderByteCode.pShaderBytecode, ShaderByteCode.BytecodeLength, IID_PPV_ARGS(&m_rootSignature)));
		const D3D12_COMPUTE_PIPELINE_STATE_DESC computePipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS = ShaderByteCode };
		ThrowIfFailed(pDevice->CreateComputePipelineState(&computePipelineStateDesc, IID_PPV_ARGS(&m_pipelineState)));
	}

	void SetConstants(const GraphicsSettings& graphicsSettings) noexcept { m_GPUBuffers.GraphicsSettings.At(0) = graphicsSettings; }

	void Render(ID3D12GraphicsCommandList* pCommandList, const TopLevelAccelerationStructure& topLevelAccelerationStructure) {
		pCommandList->SetComputeRootSignature(m_rootSignature.Get());
		pCommandList->SetComputeRootShaderResourceView(0, topLevelAccelerationStructure.GetBuffer()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootConstantBufferView(1, m_GPUBuffers.GraphicsSettings.GetResource()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootConstantBufferView(2, GPUBuffers.InSceneData->GetResource()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootConstantBufferView(3, GPUBuffers.InCamera->GetResource()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootShaderResourceView(4, GPUBuffers.InInstanceData->GetResource()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootShaderResourceView(5, GPUBuffers.InObjectData->GetResource()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootDescriptorTable(6, GPUDescriptors.InPreviousLinearDepth);
		pCommandList->SetComputeRootDescriptorTable(7, GPUDescriptors.InPreviousBaseColorMetalness);
		pCommandList->SetComputeRootDescriptorTable(8, GPUDescriptors.InPreviousNormalRoughness);
		pCommandList->SetComputeRootDescriptorTable(9, GPUDescriptors.InPreviousGeometricNormals);
		pCommandList->SetComputeRootDescriptorTable(10, GPUDescriptors.OutColor);
		pCommandList->SetComputeRootDescriptorTable(11, GPUDescriptors.OutLinearDepth);
		pCommandList->SetComputeRootDescriptorTable(12, GPUDescriptors.OutNormalizedDepth);
		pCommandList->SetComputeRootDescriptorTable(13, GPUDescriptors.OutMotionVectors);
		pCommandList->SetComputeRootDescriptorTable(14, GPUDescriptors.OutBaseColorMetalness);
		pCommandList->SetComputeRootDescriptorTable(15, GPUDescriptors.OutEmissiveColor);
		pCommandList->SetComputeRootDescriptorTable(16, GPUDescriptors.OutNormalRoughness);
		pCommandList->SetComputeRootDescriptorTable(17, GPUDescriptors.OutGeometricNormals);
		pCommandList->SetComputeRootDescriptorTable(18, GPUDescriptors.OutNoisyDiffuse);
		pCommandList->SetComputeRootDescriptorTable(19, GPUDescriptors.OutNoisySpecular);
		pCommandList->SetComputeRootShaderResourceView(20, GPUBuffers.InLightInfo ? GPUBuffers.InLightInfo->GetResource()->GetGPUVirtualAddress() : NULL);
		pCommandList->SetComputeRootShaderResourceView(21, GPUBuffers.InLightIndices ? GPUBuffers.InLightIndices->GetResource()->GetGPUVirtualAddress() : NULL);
		pCommandList->SetComputeRootDescriptorTable(22, GPUDescriptors.InNeighborOffsets);
		pCommandList->SetComputeRootUnorderedAccessView(23, GPUBuffers.OutDIReservoir ? GPUBuffers.OutDIReservoir->GetResource()->GetGPUVirtualAddress() : NULL);
		pCommandList->SetPipelineState(m_pipelineState.Get());
		const auto renderSize = m_GPUBuffers.GraphicsSettings.At(0).RenderSize;
		pCommandList->Dispatch((renderSize.x + 15) / 16, (renderSize.y + 15) / 16, 1);
	}

private:
	struct { ConstantBuffer<GraphicsSettings> GraphicsSettings; } m_GPUBuffers;

	ComPtr<ID3D12RootSignature> m_rootSignature;
	ComPtr<ID3D12PipelineState> m_pipelineState;
};
