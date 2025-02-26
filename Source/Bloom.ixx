module;

#include <memory>
#include <ranges>

#include "directxtk12/DirectXHelpers.h"

#include "Shaders/Bloom.dxil.h"

export module PostProcessing.Bloom;

import CommandList;
import DeviceContext;
import ErrorHelpers;
import PostProcessing.Merge;
import Texture;

using namespace DirectX;
using namespace ErrorHelpers;
using namespace Microsoft::WRL;
using namespace std;

namespace {
	constexpr auto operator==(XMUINT2 a, XMUINT2 b) { return a.x == b.x && a.y == b.y; }
	constexpr XMUINT2 operator/(XMUINT2 a, uint32_t b) { return { a.x / b, a.y / b }; }
}

export namespace PostProcessing {
	struct Bloom {
		struct Constants { float Strength; };

		explicit Bloom(const DeviceContext& deviceContext) noexcept(false) : m_merge(deviceContext) {
			constexpr D3D12_SHADER_BYTECODE ShaderByteCode{ g_Bloom_dxil, size(g_Bloom_dxil) };

			ThrowIfFailed(deviceContext.Device->CreateRootSignature(0, ShaderByteCode.pShaderBytecode, ShaderByteCode.BytecodeLength, IID_PPV_ARGS(&m_rootSignature)));

			const D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS = ShaderByteCode };
			ThrowIfFailed(deviceContext.Device->CreateComputePipelineState(&pipelineStateDesc, IID_PPV_ARGS(&m_pipelineState)));
			m_pipelineState->SetName(L"Bloom");
		}

		void SetTextures(Texture& input, Texture& output) {
			const auto a = m_textures.Input == &input, b = m_textures.Output == &output;
			if (a && b) {
				return;
			}
			if (!a) {
				m_textures.Input = &input;
				input.CreateSRV();
			}
			if (!b) {
				m_textures.Output = &output;
				output.CreateUAV();
			}

			const auto desc = input->GetDesc();
			if (const auto size = GetTextureSize(input) / 2;
				!m_textures.Blur1
				|| desc.Format != m_textures.Blur1->GetNative()->GetDesc().Format
				|| size != GetTextureSize(*m_textures.Blur1)) {
				const auto CreateTexture = [&](auto& texture) {
					texture = make_unique<Texture>(
						input.GetDeviceContext(),
						Texture::CreationDesc{
							.Format = desc.Format,
							.Width = size.x,
							.Height = size.y,
							.MipLevels = BlurMipLevels
						}.AsUnorderedAccess()
						);
					texture->CreateSRV();
					for (const auto i : views::iota(static_cast<UINT16>(0), BlurMipLevels)) {
						texture->CreateUAV(i);
					}
				};
				CreateTexture(m_textures.Blur1);
				CreateTexture(m_textures.Blur2);
			}
		}

		void Process(CommandList& commandList, Constants constants) {
			auto& input = *m_textures.Input, & output = *m_textures.Output;
			auto blur1 = m_textures.Blur1.get(), blur2 = m_textures.Blur2.get();

			commandList->SetComputeRootSignature(m_rootSignature.Get());
			commandList->SetPipelineState(m_pipelineState.Get());

			struct {
				uint32_t InputMipLevel;
				float UpsamplingFilterRadius;
			} _constants{};
			auto outputMipLevel = 0;

			const auto Dispatch = [&](Texture& input, Texture& output) {
				commandList.SetState(input, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
				commandList.SetState(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

				commandList->SetComputeRoot32BitConstants(0, sizeof(_constants) / 4, &_constants, 0);
				commandList->SetComputeRootDescriptorTable(1, input.GetSRVDescriptor());
				commandList->SetComputeRootDescriptorTable(2, output.GetUAVDescriptor(outputMipLevel));

				const auto size = GetTextureSize(output);
				const auto scale = 1 << outputMipLevel;
				commandList->Dispatch((size.x / scale + 15) / 16, (size.y / scale + 15) / 16, 1);
			};

			Dispatch(input, *blur1);

			for (const auto i : views::iota(0, (BlurMipLevels - 1) * 2)) {
				_constants.InputMipLevel = outputMipLevel;
				if (i < BlurMipLevels - 1) {
					outputMipLevel++;
				}
				else {
					outputMipLevel--;
					_constants.UpsamplingFilterRadius = 5e-3f;
				}

				Dispatch(*blur1, *blur2);

				swap(blur1, blur2);
			}

			m_merge.Textures = {
				.Input1 = &input,
				.Input2 = blur1,
				.Output = &output
			};

			m_merge.Process(commandList, { .Weight1 = 1 - constants.Strength, .Weight2 = constants.Strength });
		}

	private:
		static constexpr UINT16 BlurMipLevels = 5;

		Merge m_merge;

		ComPtr<ID3D12RootSignature> m_rootSignature;
		ComPtr<ID3D12PipelineState> m_pipelineState;

		struct {
			Texture* Input, * Output;
			unique_ptr<Texture> Blur1, Blur2;
		} m_textures{};
	};
}
