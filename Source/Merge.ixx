module;

#include "directxtk12/DirectXHelpers.h"

#include "Shaders/Merge.dxil.h"

export module PostProcessing.Merge;

import CommandList;
import DeviceContext;
import ErrorHelpers;
import Texture;

using namespace DirectX;
using namespace ErrorHelpers;
using namespace Microsoft::WRL;
using namespace std;

export namespace PostProcessing {
	struct Merge {
		struct Constants { float Weight1, Weight2; };

		struct { Texture* Input1, * Input2, * Output; } Textures{};

		Merge(const Merge&) = delete;
		Merge& operator=(const Merge&) = delete;

		explicit Merge(const DeviceContext& deviceContext) noexcept(false) {
			constexpr D3D12_SHADER_BYTECODE ShaderByteCode{ g_Merge_dxil, size(g_Merge_dxil) };

			ThrowIfFailed(deviceContext.Device->CreateRootSignature(0, ShaderByteCode.pShaderBytecode, ShaderByteCode.BytecodeLength, IID_PPV_ARGS(&m_rootSignature)));

			const D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS = ShaderByteCode };
			ThrowIfFailed(deviceContext.Device->CreateComputePipelineState(&pipelineStateDesc, IID_PPV_ARGS(&m_pipelineState)));
			m_pipelineState->SetName(L"Merge");
		}

		void Process(CommandList& commandList, Constants constants) {
			commandList->SetComputeRootSignature(m_rootSignature.Get());
			commandList->SetPipelineState(m_pipelineState.Get());

			commandList.SetState(*Textures.Input1, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
			commandList.SetState(*Textures.Input2, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
			commandList.SetState(*Textures.Output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

			commandList->SetComputeRoot32BitConstants(0, sizeof(constants) / 4, &constants, 0);
			commandList->SetComputeRootDescriptorTable(1, Textures.Input1->GetSRVDescriptor());
			commandList->SetComputeRootDescriptorTable(2, Textures.Input2->GetSRVDescriptor());
			commandList->SetComputeRootDescriptorTable(3, Textures.Output->GetUAVDescriptor());

			const auto size = GetTextureSize(*Textures.Output);
			commandList->Dispatch((size.x + 15) / 16, (size.y + 15) / 16, 1);
		}

	private:
		ComPtr<ID3D12RootSignature> m_rootSignature;
		ComPtr<ID3D12PipelineState> m_pipelineState;
	};
}
