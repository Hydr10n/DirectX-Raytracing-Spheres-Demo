module;

#include "directx/d3dx12.h"

#include <DirectXMath.h>

#include "Shaders/Raytracing.dxil.h"

export module Raytracing;

import Camera;
import CommonShaderData;
import ErrorHelpers;
import GPUBuffer;
import NRD;
import RaytracingHelpers;

using namespace DirectX;
using namespace DirectX::RaytracingHelpers;
using namespace ErrorHelpers;
using namespace Microsoft::WRL;
using namespace std;

export struct Raytracing {
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
		NRDSettings NRD;
	};

	struct {
		shared_ptr<ConstantBuffer<SceneData>> InSceneData;
		shared_ptr<ConstantBuffer<Camera>> InCamera;
		shared_ptr<UploadBuffer<InstanceData>> InInstanceData;
		shared_ptr<UploadBuffer<ObjectData>> InObjectData;
	} GPUBuffers;

	struct {
		D3D12_GPU_DESCRIPTOR_HANDLE
			OutColor,
			OutLinearDepth, OutNormalizedDepth,
			OutMotionVectors,
			OutBaseColorMetalness,
			OutEmissiveColor,
			OutNormalRoughness,
			OutNoisyDiffuse, OutNoisySpecular;
	} GPUDescriptors{};

	Raytracing(ID3D12Device* pDevice) noexcept(false) : m_GPUBuffers{ .GraphicsSettings = ConstantBuffer<GraphicsSettings>(pDevice) } {
		constexpr D3D12_SHADER_BYTECODE ShaderByteCode{ g_Raytracing_dxil, size(g_Raytracing_dxil) };
		ThrowIfFailed(pDevice->CreateRootSignature(0, ShaderByteCode.pShaderBytecode, ShaderByteCode.BytecodeLength, IID_PPV_ARGS(&m_rootSignature)));
		const D3D12_COMPUTE_PIPELINE_STATE_DESC computePipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS = ShaderByteCode };
		ThrowIfFailed(pDevice->CreateComputePipelineState(&computePipelineStateDesc, IID_PPV_ARGS(&m_pipelineState)));
	}

	void SetConstants(const GraphicsSettings& graphicsSettings) noexcept { m_GPUBuffers.GraphicsSettings.GetData() = graphicsSettings; }

	void Render(ID3D12GraphicsCommandList* pCommandList, const TopLevelAccelerationStructure& topLevelAccelerationStructure) {
		pCommandList->SetComputeRootSignature(m_rootSignature.Get());
		pCommandList->SetComputeRootShaderResourceView(0, topLevelAccelerationStructure.GetBuffer()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootConstantBufferView(1, m_GPUBuffers.GraphicsSettings.GetResource()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootConstantBufferView(2, GPUBuffers.InSceneData->GetResource()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootConstantBufferView(3, GPUBuffers.InCamera->GetResource()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootShaderResourceView(4, GPUBuffers.InInstanceData->GetResource()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootShaderResourceView(5, GPUBuffers.InObjectData->GetResource()->GetGPUVirtualAddress());
		pCommandList->SetComputeRootDescriptorTable(6, GPUDescriptors.OutColor);
		pCommandList->SetComputeRootDescriptorTable(7, GPUDescriptors.OutLinearDepth);
		pCommandList->SetComputeRootDescriptorTable(8, GPUDescriptors.OutNormalizedDepth);
		pCommandList->SetComputeRootDescriptorTable(9, GPUDescriptors.OutMotionVectors);
		pCommandList->SetComputeRootDescriptorTable(10, GPUDescriptors.OutBaseColorMetalness);
		pCommandList->SetComputeRootDescriptorTable(11, GPUDescriptors.OutEmissiveColor);
		pCommandList->SetComputeRootDescriptorTable(12, GPUDescriptors.OutNormalRoughness);
		pCommandList->SetComputeRootDescriptorTable(13, GPUDescriptors.OutNoisyDiffuse);
		pCommandList->SetComputeRootDescriptorTable(14, GPUDescriptors.OutNoisySpecular);
		pCommandList->SetPipelineState(m_pipelineState.Get());
		const auto renderSize = m_GPUBuffers.GraphicsSettings.GetData().RenderSize;
		pCommandList->Dispatch((renderSize.x + 15) / 16, (renderSize.y + 15) / 16, 1);
	}

private:
	struct { ConstantBuffer<GraphicsSettings> GraphicsSettings; } m_GPUBuffers;

	ComPtr<ID3D12RootSignature> m_rootSignature;
	ComPtr<ID3D12PipelineState> m_pipelineState;
};
