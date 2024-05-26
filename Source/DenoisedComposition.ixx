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
		} Textures{};

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
					CD3DX12_RESOURCE_BARRIER::Transition(*Textures.LinearDepth, Textures.LinearDepth->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(*Textures.BaseColorMetalness, Textures.BaseColorMetalness->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(*Textures.EmissiveColor, Textures.EmissiveColor->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(*Textures.NormalRoughness, Textures.NormalRoughness->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(*Textures.DenoisedDiffuse, Textures.DenoisedDiffuse->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(*Textures.DenoisedSpecular, Textures.DenoisedSpecular->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(*Textures.Color, Textures.Color->GetState(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
				}
			);

			pCommandList->SetComputeRootSignature(m_rootSignature.Get());
			pCommandList->SetPipelineState(m_pipelineState.Get());

			pCommandList->SetComputeRoot32BitConstants(0, sizeof(Constants) / 4, &Constants, 0);
			pCommandList->SetComputeRootConstantBufferView(1, GPUBuffers.Camera->GetNative()->GetGPUVirtualAddress());
			pCommandList->SetComputeRootDescriptorTable(2, Textures.LinearDepth->GetSRVDescriptor().GPUHandle);
			pCommandList->SetComputeRootDescriptorTable(3, Textures.BaseColorMetalness->GetSRVDescriptor().GPUHandle);
			pCommandList->SetComputeRootDescriptorTable(4, Textures.EmissiveColor->GetSRVDescriptor().GPUHandle);
			pCommandList->SetComputeRootDescriptorTable(5, Textures.NormalRoughness->GetSRVDescriptor().GPUHandle);
			pCommandList->SetComputeRootDescriptorTable(6, Textures.DenoisedDiffuse->GetSRVDescriptor().GPUHandle);
			pCommandList->SetComputeRootDescriptorTable(7, Textures.DenoisedSpecular->GetSRVDescriptor().GPUHandle);
			pCommandList->SetComputeRootDescriptorTable(8, Textures.Color->GetUAVDescriptor().GPUHandle);

			pCommandList->Dispatch((Constants.RenderSize.x + 15) / 16, (Constants.RenderSize.y + 15) / 16, 1);
		}

	private:
		ComPtr<ID3D12RootSignature> m_rootSignature;
		ComPtr<ID3D12PipelineState> m_pipelineState;
	};
}
