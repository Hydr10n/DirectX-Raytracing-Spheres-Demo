module;

#include <DirectXMath.h>

#include "directx/d3dx12.h"

#include "directxtk12/DirectXHelpers.h"

#include "Shaders/DenoisedComposition.dxil.h"

export module PostProcessing.DenoisedComposition;

import Camera;
import ErrorHelpers;
import GPUBuffer;
import NRD;
import RenderTexture;

using namespace DirectX;
using namespace ErrorHelpers;
using namespace Microsoft::WRL;
using namespace std;

export namespace PostProcessing {
	struct DenoisedComposition {
		struct {
			XMUINT2 RenderSize;
			NRDDenoiser NRDDenoiser;
		} Constants{};

		struct { ConstantBuffer<Camera>* InCamera; } GPUBuffers{};

		struct {
			RenderTexture
				* InLinearDepth,
				* InBaseColorMetalness,
				* InEmissiveColor,
				* InNormalRoughness,
				* InDenoisedDiffuse,
				* InDenoisedSpecular,
				* OutColor;
		} RenderTextures{};

		explicit DenoisedComposition(ID3D12Device* pDevice) noexcept(false) {
			constexpr D3D12_SHADER_BYTECODE ShaderByteCode{ g_DenoisedComposition_dxil, size(g_DenoisedComposition_dxil) };
			ThrowIfFailed(pDevice->CreateRootSignature(0, ShaderByteCode.pShaderBytecode, ShaderByteCode.BytecodeLength, IID_PPV_ARGS(&m_rootSignature)));
			const D3D12_COMPUTE_PIPELINE_STATE_DESC computePipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS = ShaderByteCode };
			ThrowIfFailed(pDevice->CreateComputePipelineState(&computePipelineStateDesc, IID_PPV_ARGS(&m_pipelineState)));
			m_pipelineState->SetName(L"DenoisedComposition");
		}

		void Process(ID3D12GraphicsCommandList* pCommandList) {
			const ScopedBarrier scopedBarrier(
				pCommandList,
				{
					CD3DX12_RESOURCE_BARRIER::Transition(RenderTextures.InLinearDepth->GetResource(), RenderTextures.InLinearDepth->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(RenderTextures.InBaseColorMetalness->GetResource(), RenderTextures.InBaseColorMetalness->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(RenderTextures.InEmissiveColor->GetResource(), RenderTextures.InEmissiveColor->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(RenderTextures.InNormalRoughness->GetResource(), RenderTextures.InNormalRoughness->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(RenderTextures.InDenoisedDiffuse->GetResource(), RenderTextures.InDenoisedDiffuse->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(RenderTextures.InDenoisedSpecular->GetResource(), RenderTextures.InDenoisedSpecular->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(RenderTextures.OutColor->GetResource(), RenderTextures.OutColor->GetState(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
				}
			);
			pCommandList->SetPipelineState(m_pipelineState.Get());
			pCommandList->SetComputeRootSignature(m_rootSignature.Get());
			pCommandList->SetComputeRoot32BitConstants(0, sizeof(Constants) / 4, &Constants, 0);
			pCommandList->SetComputeRootConstantBufferView(1, GPUBuffers.InCamera->GetResource()->GetGPUVirtualAddress());
			pCommandList->SetComputeRootDescriptorTable(2, RenderTextures.InLinearDepth->GetSRVDescriptor().GPUHandle);
			pCommandList->SetComputeRootDescriptorTable(3, RenderTextures.InBaseColorMetalness->GetSRVDescriptor().GPUHandle);
			pCommandList->SetComputeRootDescriptorTable(4, RenderTextures.InEmissiveColor->GetSRVDescriptor().GPUHandle);
			pCommandList->SetComputeRootDescriptorTable(5, RenderTextures.InNormalRoughness->GetSRVDescriptor().GPUHandle);
			pCommandList->SetComputeRootDescriptorTable(6, RenderTextures.InDenoisedDiffuse->GetSRVDescriptor().GPUHandle);
			pCommandList->SetComputeRootDescriptorTable(7, RenderTextures.InDenoisedSpecular->GetSRVDescriptor().GPUHandle);
			pCommandList->SetComputeRootDescriptorTable(8, RenderTextures.OutColor->GetUAVDescriptor().GPUHandle);
			pCommandList->Dispatch((Constants.RenderSize.x + 15) / 16, (Constants.RenderSize.y + 15) / 16, 1);
		}

	private:
		ComPtr<ID3D12RootSignature> m_rootSignature;
		ComPtr<ID3D12PipelineState> m_pipelineState;
	};
}
