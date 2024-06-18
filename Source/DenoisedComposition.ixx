module;

#include "directx/d3d12.h"

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
		struct Constants {
			XMUINT2 RenderSize;
			NRDDenoiser NRDDenoiser;
		};

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

		DenoisedComposition(const DenoisedComposition&) = delete;
		DenoisedComposition& operator=(const DenoisedComposition&) = delete;

		explicit DenoisedComposition(ID3D12Device* pDevice) noexcept(false) {
			constexpr D3D12_SHADER_BYTECODE ShaderByteCode{ g_DenoisedComposition_dxil, size(g_DenoisedComposition_dxil) };

			ThrowIfFailed(pDevice->CreateRootSignature(0, ShaderByteCode.pShaderBytecode, ShaderByteCode.BytecodeLength, IID_PPV_ARGS(&m_rootSignature)));

			const D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS = ShaderByteCode };
			ThrowIfFailed(pDevice->CreateComputePipelineState(&pipelineStateDesc, IID_PPV_ARGS(&m_pipelineState)));
			m_pipelineState->SetName(L"DenoisedComposition");
		}

		void Process(ID3D12GraphicsCommandList* pCommandList, const Constants& constants) {
			const ScopedBarrier scopedBarrier(
				pCommandList,
				{
					Textures.LinearDepth->TransitionBarrier(D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
					Textures.BaseColorMetalness->TransitionBarrier(D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
					Textures.EmissiveColor->TransitionBarrier(D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
					Textures.NormalRoughness->TransitionBarrier(D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
					Textures.DenoisedDiffuse->TransitionBarrier(D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
					Textures.DenoisedSpecular->TransitionBarrier(D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
					Textures.Color->TransitionBarrier(D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
				}
			);

			pCommandList->SetComputeRootSignature(m_rootSignature.Get());
			pCommandList->SetPipelineState(m_pipelineState.Get());

			pCommandList->SetComputeRoot32BitConstants(0, sizeof(constants) / 4, &constants, 0);
			pCommandList->SetComputeRootConstantBufferView(1, GPUBuffers.Camera->GetNative()->GetGPUVirtualAddress());
			pCommandList->SetComputeRootDescriptorTable(2, Textures.LinearDepth->GetSRVDescriptor().GPUHandle);
			pCommandList->SetComputeRootDescriptorTable(3, Textures.BaseColorMetalness->GetSRVDescriptor().GPUHandle);
			pCommandList->SetComputeRootDescriptorTable(4, Textures.EmissiveColor->GetSRVDescriptor().GPUHandle);
			pCommandList->SetComputeRootDescriptorTable(5, Textures.NormalRoughness->GetSRVDescriptor().GPUHandle);
			pCommandList->SetComputeRootDescriptorTable(6, Textures.DenoisedDiffuse->GetSRVDescriptor().GPUHandle);
			pCommandList->SetComputeRootDescriptorTable(7, Textures.DenoisedSpecular->GetSRVDescriptor().GPUHandle);
			pCommandList->SetComputeRootDescriptorTable(8, Textures.Color->GetUAVDescriptor().GPUHandle);

			pCommandList->Dispatch((constants.RenderSize.x + 15) / 16, (constants.RenderSize.y + 15) / 16, 1);
		}

	private:
		ComPtr<ID3D12RootSignature> m_rootSignature;
		ComPtr<ID3D12PipelineState> m_pipelineState;
	};
}
