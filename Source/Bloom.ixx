module;

#include <memory>
#include <ranges>

#include "directxtk12/DirectXHelpers.h"

#include "Shaders/Bloom.dxil.h"

export module PostProcessing.Bloom;

import DescriptorHeap;
import ErrorHelpers;
import PostProcessing.Merge;
import Texture;

using namespace DirectX;
using namespace ErrorHelpers;
using namespace Microsoft::WRL;
using namespace std;

namespace {
	auto operator==(XMUINT2 a, XMUINT2 b) { return a.x == b.x && a.y == b.y; }
	XMUINT2 operator/(XMUINT2 a, UINT b) { return { a.x / b, a.y / b }; }
}

export namespace PostProcessing {
	struct Bloom {
		struct Constants { float Strength; };

		explicit Bloom(ID3D12Device* pDevice) noexcept(false) : m_device(pDevice), m_descriptorHeap(pDevice, DescriptorIndex::Reserve + (1 + BlurMipLevels) * 2), m_merge(pDevice) {
			constexpr D3D12_SHADER_BYTECODE ShaderByteCode{ g_Bloom_dxil, size(g_Bloom_dxil) };

			ThrowIfFailed(pDevice->CreateRootSignature(0, ShaderByteCode.pShaderBytecode, ShaderByteCode.BytecodeLength, IID_PPV_ARGS(&m_rootSignature)));

			const D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS = ShaderByteCode };
			ThrowIfFailed(pDevice->CreateComputePipelineState(&pipelineStateDesc, IID_PPV_ARGS(&m_pipelineState)));
			m_pipelineState->SetName(L"Bloom");
		}

		void SetTextures(const Texture& input, const Texture& output) {
			const auto a = m_textures.Input && *m_textures.Input == input, b = m_textures.Output && *m_textures.Output == output;
			if (a) {
				m_textures.Input->SetState(input.GetState());
			}
			else {
				m_textures.Input = make_unique<Texture>(input, input.GetState());
				m_textures.Input->CreateSRV(m_descriptorHeap, DescriptorIndex::Input);
			}
			if (b) {
				m_textures.Output->SetState(output.GetState());
			}
			else {
				m_textures.Output = make_unique<Texture>(output, output.GetState());
				m_textures.Output->CreateUAV(m_descriptorHeap, DescriptorIndex::Output);
			}
			if (a && b) return;

			const auto desc = input->GetDesc();
			const auto size = GetTextureSize(input) / 2;
			if (!m_textures.Blur1 || desc.Format != m_textures.Blur1->GetNative()->GetDesc().Format || size != GetTextureSize(*m_textures.Blur1)) {
				UINT index = DescriptorIndex::Reserve;
				const auto CreateTexture = [&](unique_ptr<Texture>& texture) {
					texture = make_unique<Texture>(m_device, desc.Format, size, BlurMipLevels);
					texture->CreateSRV(m_descriptorHeap, index++);
					for (const auto i : views::iota(static_cast<UINT16>(0), BlurMipLevels)) texture->CreateUAV(m_descriptorHeap, index++, i);
				};
				CreateTexture(m_textures.Blur1);
				CreateTexture(m_textures.Blur2);
			}
		}

		void Process(ID3D12GraphicsCommandList* pCommandList, Constants constants) {
			auto& input = *m_textures.Input, & output = *m_textures.Output;
			auto blur1 = m_textures.Blur1.get(), blur2 = m_textures.Blur2.get();

			const auto descriptorHeap = m_descriptorHeap.Heap();
			pCommandList->SetDescriptorHeaps(1, &descriptorHeap);

			pCommandList->SetComputeRootSignature(m_rootSignature.Get());
			pCommandList->SetPipelineState(m_pipelineState.Get());

			struct {
				UINT InputMipLevel;
				float UpsamplingFilterRadius;
			} _constants{};
			auto outputMipLevel = 0;

			const auto Dispatch = [&](const Texture& input, const Texture& output) {
				const ScopedBarrier scopedBarrier(
					pCommandList,
					{
						input.TransitionBarrier(D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
						output.TransitionBarrier(D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
					}
				);

				pCommandList->SetComputeRoot32BitConstants(0, sizeof(_constants) / 4, &_constants, 0);
				pCommandList->SetComputeRootDescriptorTable(1, input.GetSRVDescriptor().GPUHandle);
				pCommandList->SetComputeRootDescriptorTable(2, output.GetUAVDescriptor(outputMipLevel).GPUHandle);

				const auto size = GetTextureSize(output);
				const auto scale = 1 << outputMipLevel;
				pCommandList->Dispatch((size.x / scale + 15) / 16, (size.y / scale + 15) / 16, 1);
			};

			Dispatch(input, *blur1);

			for (const auto i : views::iota(0, (BlurMipLevels - 1) * 2)) {
				_constants.InputMipLevel = outputMipLevel;
				if (i < BlurMipLevels - 1) outputMipLevel++;
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

			m_merge.Process(pCommandList, { .Weight1 = 1 - constants.Strength, .Weight2 = constants.Strength });
		}

	private:
		static constexpr UINT16 BlurMipLevels = 5;

		struct DescriptorIndex {
			enum {
				Input,
				Output,
				Reserve
			};
		};

		ID3D12Device* m_device;

		DescriptorHeapEx m_descriptorHeap;

		Merge m_merge;

		ComPtr<ID3D12RootSignature> m_rootSignature;
		ComPtr<ID3D12PipelineState> m_pipelineState;

		struct { unique_ptr<Texture> Input, Output, Blur1, Blur2; } m_textures{};
	};
}
