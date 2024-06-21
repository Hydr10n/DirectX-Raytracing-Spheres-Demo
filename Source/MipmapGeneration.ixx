module;

#include <memory>
#include <ranges>

#include "directxtk12/DirectXHelpers.h"

#include "Shaders/MipmapGeneration.dxil.h"

export module PostProcessing.MipmapGeneration;

import DescriptorHeap;
import ErrorHelpers;
import Texture;

using namespace DirectX;
using namespace ErrorHelpers;
using namespace Microsoft::WRL;
using namespace std;

export namespace PostProcessing {
	struct MipmapGeneration {
		explicit MipmapGeneration(ID3D12Device* pDevice) noexcept(false) : m_descriptorHeap(pDevice, 16) {
			constexpr D3D12_SHADER_BYTECODE ShaderByteCode{ g_MipmapGeneration_dxil, size(g_MipmapGeneration_dxil) };

			ThrowIfFailed(pDevice->CreateRootSignature(0, ShaderByteCode.pShaderBytecode, ShaderByteCode.BytecodeLength, IID_PPV_ARGS(&m_rootSignature)));

			const D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS = ShaderByteCode };
			ThrowIfFailed(pDevice->CreateComputePipelineState(&pipelineStateDesc, IID_PPV_ARGS(&m_pipelineState)));
			m_pipelineState->SetName(L"MipmapGeneration");
		}

		void SetTexture(const Texture& texture) {
			if (m_texture && *m_texture == texture) {
				m_texture->SetState(texture.GetState());
				return;
			}

			m_texture = make_unique<Texture>(texture, texture.GetState());
			for (const auto i : views::iota(0u, texture->GetDesc().MipLevels)) m_texture->CreateUAV(m_descriptorHeap, i, i);
		}

		void Process(ID3D12GraphicsCommandList* pCommandList) {
			const ScopedBarrier scopedBarrier(pCommandList, { m_texture->TransitionBarrier(D3D12_RESOURCE_STATE_UNORDERED_ACCESS) });

			const auto descriptorHeap = m_descriptorHeap.Heap();
			pCommandList->SetDescriptorHeaps(1, &descriptorHeap);

			pCommandList->SetComputeRootSignature(m_rootSignature.Get());
			pCommandList->SetPipelineState(m_pipelineState.Get());

			const auto size = GetTextureSize(*m_texture);
			for (UINT mipLevel = 0, mipLevels = m_texture->GetNative()->GetDesc().MipLevels; mipLevel < mipLevels; mipLevel += 5) {
				const struct { UINT MipLevel, MipLevels; } constants{ mipLevel, mipLevels };
				pCommandList->SetComputeRoot32BitConstants(0, sizeof(constants) / 4, &constants, 0);

				pCommandList->Dispatch((max(1u, size.x >> mipLevel) + 31) / 32, (max(1u, size.y >> mipLevel) + 31) / 32, 1);

				m_texture->InsertUAVBarrier(pCommandList);
			}
		}

	private:
		DescriptorHeapEx m_descriptorHeap;

		ComPtr<ID3D12RootSignature> m_rootSignature;
		ComPtr<ID3D12PipelineState> m_pipelineState;

		unique_ptr<Texture> m_texture;
	};
}
