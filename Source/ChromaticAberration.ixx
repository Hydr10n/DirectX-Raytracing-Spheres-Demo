module;

#include "directx/d3dx12.h"

#include "directxtk12/DirectXHelpers.h"

#include "Shaders/ChromaticAberration.dxil.h"

export module PostProcessing.ChromaticAberration;

import ErrorHelpers;
import Texture;

using namespace DirectX;
using namespace ErrorHelpers;
using namespace Microsoft::WRL;
using namespace std;

export namespace PostProcessing {
	struct ChromaticAberration {
		struct Constants {
			XMFLOAT2 FocusUV{ 0.5f, 0.5f };
			XMFLOAT3 Offsets{ 3e-3f, 3e-3f, -3e-3f };
		};

		struct { Texture* Input, * Output; } Textures{};

		ChromaticAberration(const ChromaticAberration&) = delete;
		ChromaticAberration& operator=(const ChromaticAberration&) = delete;

		explicit ChromaticAberration(ID3D12Device* pDevice) noexcept(false) {
			constexpr D3D12_SHADER_BYTECODE ShaderByteCode{ g_ChromaticAberration_dxil, size(g_ChromaticAberration_dxil) };

			ThrowIfFailed(pDevice->CreateRootSignature(0, ShaderByteCode.pShaderBytecode, ShaderByteCode.BytecodeLength, IID_PPV_ARGS(&m_rootSignature)));

			const D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS = ShaderByteCode };
			ThrowIfFailed(pDevice->CreateComputePipelineState(&pipelineStateDesc, IID_PPV_ARGS(&m_pipelineState)));
			m_pipelineState->SetName(L"ChromaticAberration");
		}

		void Process(ID3D12GraphicsCommandList* pCommandList, const Constants& constants = {}) {
			const ScopedBarrier scopedBarrier(
				pCommandList,
				{
					CD3DX12_RESOURCE_BARRIER::Transition(*Textures.Input, Textures.Input->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(*Textures.Output, Textures.Output->GetState(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
				}
			);

			pCommandList->SetComputeRootSignature(m_rootSignature.Get());
			pCommandList->SetPipelineState(m_pipelineState.Get());

			pCommandList->SetComputeRoot32BitConstants(0, sizeof(constants) / 4, &constants, 0);
			pCommandList->SetComputeRootDescriptorTable(1, Textures.Input->GetSRVDescriptor().GPUHandle);
			pCommandList->SetComputeRootDescriptorTable(2, Textures.Output->GetUAVDescriptor().GPUHandle);

			const auto size = GetTextureSize(*Textures.Output);
			pCommandList->Dispatch((size.x + 15) / 16, (size.y + 15) / 16, 1);
		}

	private:
		ComPtr<ID3D12RootSignature> m_rootSignature;
		ComPtr<ID3D12PipelineState> m_pipelineState;
	};
}
