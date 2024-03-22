module;

#include <ranges>

#include "directx/d3dx12.h"

#include "directxtk12/DescriptorHeap.h"
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
		struct { float Strength; } Constants{};

		explicit Bloom(ID3D12Device* pDevice) noexcept(false) : m_device(pDevice), m_descriptorHeap(pDevice, DescriptorIndex::Reserve + (1 + BlurMipLevels) * 2), m_merge(pDevice) {
			constexpr D3D12_SHADER_BYTECODE ShaderByteCode{ g_Bloom_dxil, size(g_Bloom_dxil) };
			ThrowIfFailed(pDevice->CreateRootSignature(0, ShaderByteCode.pShaderBytecode, ShaderByteCode.BytecodeLength, IID_PPV_ARGS(&m_rootSignature)));
			const D3D12_COMPUTE_PIPELINE_STATE_DESC computePipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS = ShaderByteCode };
			ThrowIfFailed(pDevice->CreateComputePipelineState(&computePipelineStateDesc, IID_PPV_ARGS(&m_pipelineState)));
			m_pipelineState->SetName(L"Bloom");
		}

		void SetTextures(const Texture& input, const Texture& output) {
			auto a = false, b = false;
			if (!m_renderTextures.Input || m_renderTextures.Input->GetNative() != input) {
				m_renderTextures.Input = make_unique<Texture>(input, input.GetState());
				m_renderTextures.Input->CreateSRV(m_descriptorHeap, DescriptorIndex::Input);
			}
			else if (m_renderTextures.Input && m_renderTextures.Input->GetNative() == input) {
				m_renderTextures.Input->SetState(input.GetState());
				a = true;
			}
			if (!m_renderTextures.Output || m_renderTextures.Output->GetNative() != output) {
				m_renderTextures.Output = make_unique<Texture>(output, output.GetState());
				m_renderTextures.Output->CreateUAV(m_descriptorHeap, DescriptorIndex::Output);
			}
			else if (m_renderTextures.Output && m_renderTextures.Output->GetNative() == output) {
				m_renderTextures.Output->SetState(output.GetState());
				b = true;
			}
			if (a && b) return;

			const auto desc = input->GetDesc();
			const auto size = GetTextureSize(input) / 2;
			if (!m_renderTextures.Blur1 || desc.Format != m_renderTextures.Blur1->GetNative()->GetDesc().Format || size != GetTextureSize(*m_renderTextures.Blur1)) {
				UINT index = DescriptorIndex::Reserve;
				const auto CreateTexture = [&](unique_ptr<Texture>& texture) {
					texture = make_unique<Texture>(m_device, desc.Format, size, BlurMipLevels);
					texture->CreateSRV(m_descriptorHeap, index++);
					for (const auto i : views::iota(static_cast<UINT16>(0), BlurMipLevels)) texture->CreateUAV(m_descriptorHeap, index++, i);
				};
				CreateTexture(m_renderTextures.Blur1);
				CreateTexture(m_renderTextures.Blur2);
			}
		}

		void Process(ID3D12GraphicsCommandList* pCommandList) {
			auto& input = *m_renderTextures.Input, & output = *m_renderTextures.Output;
			auto blur1 = m_renderTextures.Blur1.get(), blur2 = m_renderTextures.Blur2.get();

			const auto descriptorHeap = m_descriptorHeap.Heap();
			pCommandList->SetDescriptorHeaps(1, &descriptorHeap);

			pCommandList->SetPipelineState(m_pipelineState.Get());
			pCommandList->SetComputeRootSignature(m_rootSignature.Get());

			struct {
				UINT InputMipLevel;
				float UpsamplingFilterRadius;
			} constants{};
			auto outputMipLevel = 0;

			const auto Dispatch = [&](const Texture& input, const Texture& output) {
				const ScopedBarrier scopedBarrier(
					pCommandList,
					{
						CD3DX12_RESOURCE_BARRIER::Transition(input, input.GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
						CD3DX12_RESOURCE_BARRIER::Transition(output, output.GetState(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
					}
				);
				pCommandList->SetComputeRoot32BitConstants(0, sizeof(constants) / 4, &constants, 0);
				pCommandList->SetComputeRootDescriptorTable(1, input.GetSRVDescriptor().GPUHandle);
				pCommandList->SetComputeRootDescriptorTable(2, output.GetUAVDescriptor(outputMipLevel).GPUHandle);
				const auto size = GetTextureSize(output);
				const auto scale = 1 << outputMipLevel;
				pCommandList->Dispatch((size.x / scale + 15) / 16, (size.y / scale + 15) / 16, 1);
			};

			Dispatch(input, *blur1);

			for (const auto i : views::iota(0, (BlurMipLevels - 1) * 2)) {
				constants.InputMipLevel = outputMipLevel;
				if (i < BlurMipLevels - 1) outputMipLevel++;
				else {
					outputMipLevel--;
					constants.UpsamplingFilterRadius = 5e-3f;
				}

				Dispatch(*blur1, *blur2);

				swap(blur1, blur2);
			}

			m_merge.Constants = {
				.Weight1 = 1 - Constants.Strength,
				.Weight2 = Constants.Strength
			};

			m_merge.Textures = {
				.Input1 = &input,
				.Input2 = blur1,
				.Output = &output
			};

			m_merge.Process(pCommandList);
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

		DescriptorHeap m_descriptorHeap;

		Merge m_merge;

		ComPtr<ID3D12RootSignature> m_rootSignature;
		ComPtr<ID3D12PipelineState> m_pipelineState;

		struct { unique_ptr<Texture> Input, Output, Blur1, Blur2; } m_renderTextures{};
	};
}
