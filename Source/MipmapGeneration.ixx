module;

#include <memory>
#include <ranges>

#include "directxtk12/DirectXHelpers.h"

#include "Shaders/MipmapGeneration.dxil.h"

export module PostProcessing.MipmapGeneration;

import CommandList;
import DeviceContext;
import ErrorHelpers;
import Texture;

using namespace DirectX;
using namespace ErrorHelpers;
using namespace Microsoft::WRL;
using namespace std;

export namespace PostProcessing {
	struct MipmapGeneration {
		explicit MipmapGeneration(const DeviceContext& deviceContext) noexcept(false) {
			constexpr D3D12_SHADER_BYTECODE ShaderByteCode{ g_MipmapGeneration_dxil, size(g_MipmapGeneration_dxil) };

			ThrowIfFailed(deviceContext.Device->CreateRootSignature(0, ShaderByteCode.pShaderBytecode, ShaderByteCode.BytecodeLength, IID_PPV_ARGS(&m_rootSignature)));

			const D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS = ShaderByteCode };
			ThrowIfFailed(deviceContext.Device->CreateComputePipelineState(&pipelineStateDesc, IID_PPV_ARGS(&m_pipelineState)));
			m_pipelineState->SetName(L"MipmapGeneration");
		}

		void SetTexture(Texture& texture) {
			if (m_texture == &texture) {
				return;
			}

			for (const auto i : views::iota(0u, min<uint16_t>(texture->GetDesc().MipLevels, 16))) {
				texture.CreateUAV(i);
			}
			m_texture = &texture;
		}

		void Process(CommandList& commandList) {
			commandList->SetComputeRootSignature(m_rootSignature.Get());
			commandList->SetPipelineState(m_pipelineState.Get());

			commandList.SetState(*m_texture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

			const auto mipLevels = min<uint16_t>(m_texture->GetNative()->GetDesc().MipLevels, 16);

			struct { uint32_t MipLevelDescriptorIndices[16], MipLevels; } constants{ .MipLevels = mipLevels };
			for (const auto i : views::iota(0u, mipLevels)) {
				constants.MipLevelDescriptorIndices[i] = m_texture->GetUAVDescriptor(i);
			}
			commandList->SetComputeRoot32BitConstants(0, sizeof(constants) / 4, &constants, 0);

			auto size = GetTextureSize(*m_texture);
			for (uint16_t mipLevel = 0; mipLevel < mipLevels; mipLevel += 5) {
				commandList->SetComputeRoot32BitConstant(1, mipLevel, 0);

				commandList->Dispatch((max(size.x >> mipLevel, 1u) + 31) / 32, (max(size.y >> mipLevel, 1u) + 31) / 32, 1);

				size.x = max(1u, size.x >> 5);
				size.y = max(1u, size.y >> 5);

				commandList.SetUAVBarrier(*m_texture);
			}
		}

	private:
		ComPtr<ID3D12RootSignature> m_rootSignature;
		ComPtr<ID3D12PipelineState> m_pipelineState;

		Texture* m_texture{};
	};
}
