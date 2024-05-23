module;

#include "directx/d3dx12.h"

#include "directxtk12/DirectXHelpers.h"

#include "Shaders/DenoisedComposition.dxil.h"

export module PostProcessing.DenoisedComposition;

import Camera;
import ErrorHelpers;
import GPUBuffer;
import NRD;
import Texture;

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

		struct { ConstantBuffer<Camera>* Camera; } GPUBuffers{};

		struct {
			Texture
				* LinearDepth,
				* BaseColorMetalness,
				* EmissiveColor,
				* NormalRoughness,
				* DenoisedDiffuse,
				* DenoisedSpecular,
				* Color;
		} RenderTextures{};

		explicit DenoisedComposition(ID3D12Device* pDevice) noexcept(false) {
			constexpr D3D12_SHADER_BYTECODE ShaderByteCode{ g_DenoisedComposition_dxil, size(g_DenoisedComposition_dxil) };

			ThrowIfFailed(pDevice->CreateRootSignature(0, ShaderByteCode.pShaderBytecode, ShaderByteCode.BytecodeLength, IID_PPV_ARGS(&m_rootSignature)));

			const D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS = ShaderByteCode };
			ThrowIfFailed(pDevice->CreateComputePipelineState(&pipelineStateDesc, IID_PPV_ARGS(&m_pipelineState)));
			m_pipelineState->SetName(L"DenoisedComposition");
		}

		void Process(ID3D12GraphicsCommandList* pCommandList) {
			const ScopedBarrier scopedBarrier(
				pCommandList,
				{
					CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.LinearDepth, RenderTextures.LinearDepth->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.BaseColorMetalness, RenderTextures.BaseColorMetalness->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.EmissiveColor, RenderTextures.EmissiveColor->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.NormalRoughness, RenderTextures.NormalRoughness->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.DenoisedDiffuse, RenderTextures.DenoisedDiffuse->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.DenoisedSpecular, RenderTextures.DenoisedSpecular->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(*RenderTextures.Color, RenderTextures.Color->GetState(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
				}
			);

			pCommandList->SetComputeRootSignature(m_rootSignature.Get());
			pCommandList->SetPipelineState(m_pipelineState.Get());

			pCommandList->SetComputeRoot32BitConstants(0, sizeof(Constants) / 4, &Constants, 0);
			pCommandList->SetComputeRootConstantBufferView(1, GPUBuffers.Camera->GetNative()->GetGPUVirtualAddress());
			pCommandList->SetComputeRootDescriptorTable(2, RenderTextures.LinearDepth->GetSRVDescriptor().GPUHandle);
			pCommandList->SetComputeRootDescriptorTable(3, RenderTextures.BaseColorMetalness->GetSRVDescriptor().GPUHandle);
			pCommandList->SetComputeRootDescriptorTable(4, RenderTextures.EmissiveColor->GetSRVDescriptor().GPUHandle);
			pCommandList->SetComputeRootDescriptorTable(5, RenderTextures.NormalRoughness->GetSRVDescriptor().GPUHandle);
			pCommandList->SetComputeRootDescriptorTable(6, RenderTextures.DenoisedDiffuse->GetSRVDescriptor().GPUHandle);
			pCommandList->SetComputeRootDescriptorTable(7, RenderTextures.DenoisedSpecular->GetSRVDescriptor().GPUHandle);
			pCommandList->SetComputeRootDescriptorTable(8, RenderTextures.Color->GetUAVDescriptor().GPUHandle);

			pCommandList->Dispatch((Constants.RenderSize.x + 15) / 16, (Constants.RenderSize.y + 15) / 16, 1);
		}

	private:
		ComPtr<ID3D12RootSignature> m_rootSignature;
		ComPtr<ID3D12PipelineState> m_pipelineState;
	};
}
