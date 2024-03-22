module;

#include "directx/d3dx12.h"

#include "directxtk12/DirectXHelpers.h"

#include "Shaders/Merge.dxil.h"

export module PostProcessing.Merge;

import ErrorHelpers;
import Texture;

using namespace DirectX;
using namespace ErrorHelpers;
using namespace Microsoft::WRL;
using namespace std;

export namespace PostProcessing {
	struct Merge {
		struct { float Weight1, Weight2; } Constants{};

		struct { Texture* Input1, * Input2, * Output; } Textures{};

		explicit Merge(ID3D12Device* pDevice) noexcept(false) {
			constexpr D3D12_SHADER_BYTECODE ShaderByteCode{ g_Merge_dxil, size(g_Merge_dxil) };
			ThrowIfFailed(pDevice->CreateRootSignature(0, ShaderByteCode.pShaderBytecode, ShaderByteCode.BytecodeLength, IID_PPV_ARGS(&m_rootSignature)));
			const D3D12_COMPUTE_PIPELINE_STATE_DESC computePipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS = ShaderByteCode };
			ThrowIfFailed(pDevice->CreateComputePipelineState(&computePipelineStateDesc, IID_PPV_ARGS(&m_pipelineState)));
			m_pipelineState->SetName(L"Merge");
		}

		void Process(ID3D12GraphicsCommandList* pCommandList) {
			const ScopedBarrier scopedBarrier(
				pCommandList,
				{
					CD3DX12_RESOURCE_BARRIER::Transition(*Textures.Input1, Textures.Input1->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(*Textures.Input2, Textures.Input2->GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(*Textures.Output, Textures.Output->GetState(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
				}
			);
			pCommandList->SetPipelineState(m_pipelineState.Get());
			pCommandList->SetComputeRootSignature(m_rootSignature.Get());
			pCommandList->SetComputeRoot32BitConstants(0, sizeof(Constants) / 4, &Constants, 0);
			pCommandList->SetComputeRootDescriptorTable(1, Textures.Input1->GetSRVDescriptor().GPUHandle);
			pCommandList->SetComputeRootDescriptorTable(2, Textures.Input2->GetSRVDescriptor().GPUHandle);
			pCommandList->SetComputeRootDescriptorTable(3, Textures.Output->GetUAVDescriptor().GPUHandle);
			const auto size = GetTextureSize(*Textures.Output);
			pCommandList->Dispatch((size.x + 15) / 16, (size.y + 15) / 16, 1);
		}

	private:
		ComPtr<ID3D12RootSignature> m_rootSignature;
		ComPtr<ID3D12PipelineState> m_pipelineState;
	};
}
