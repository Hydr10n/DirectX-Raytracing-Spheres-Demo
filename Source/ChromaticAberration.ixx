module;

#include "directxtk12/DirectXHelpers.h"

#include "Shaders/ChromaticAberration.dxil.h"

export module PostProcessing.ChromaticAberration;

import CommandList;
import DeviceContext;
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

		explicit ChromaticAberration(const DeviceContext& deviceContext) noexcept(false) {
			constexpr D3D12_SHADER_BYTECODE ShaderByteCode{ g_ChromaticAberration_dxil, size(g_ChromaticAberration_dxil) };

			ThrowIfFailed(deviceContext.Device->CreateRootSignature(0, ShaderByteCode.pShaderBytecode, ShaderByteCode.BytecodeLength, IID_PPV_ARGS(&m_rootSignature)));

			const D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS = ShaderByteCode };
			ThrowIfFailed(deviceContext.Device->CreateComputePipelineState(&pipelineStateDesc, IID_PPV_ARGS(&m_pipelineState)));
			m_pipelineState->SetName(L"ChromaticAberration");
		}

		void Process(CommandList& commandList, const Constants& constants = {}) {
			commandList->SetComputeRootSignature(m_rootSignature.Get());
			commandList->SetPipelineState(m_pipelineState.Get());

			commandList.SetState(*Textures.Input, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
			commandList.SetState(*Textures.Output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

			commandList->SetComputeRoot32BitConstants(0, sizeof(constants) / 4, &constants, 0);
			commandList->SetComputeRootDescriptorTable(1, Textures.Input->GetSRVDescriptor());
			commandList->SetComputeRootDescriptorTable(2, Textures.Output->GetUAVDescriptor());

			const auto size = GetTextureSize(*Textures.Output);
			commandList->Dispatch((size.x + 15) / 16, (size.y + 15) / 16, 1);
		}

	private:
		ComPtr<ID3D12RootSignature> m_rootSignature;
		ComPtr<ID3D12PipelineState> m_pipelineState;
	};
}
