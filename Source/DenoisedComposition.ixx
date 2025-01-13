module;

#include "directxtk12/DirectXHelpers.h"

#include "Shaders/DenoisedComposition.dxil.h"

export module PostProcessing.DenoisedComposition;

import CommandList;
import DeviceContext;
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

		struct { GPUBuffer* Camera; } GPUBuffers{};

		struct {
			Texture
				* LinearDepth,
				* BaseColorMetalness,
				* NormalRoughness,
				* DenoisedDiffuse,
				* DenoisedSpecular,
				* Radiance;
		} Textures{};

		DenoisedComposition(const DenoisedComposition&) = delete;
		DenoisedComposition& operator=(const DenoisedComposition&) = delete;

		explicit DenoisedComposition(const DeviceContext& deviceContext) noexcept(false) {
			constexpr D3D12_SHADER_BYTECODE ShaderByteCode{ g_DenoisedComposition_dxil, size(g_DenoisedComposition_dxil) };

			ThrowIfFailed(deviceContext.Device->CreateRootSignature(0, ShaderByteCode.pShaderBytecode, ShaderByteCode.BytecodeLength, IID_PPV_ARGS(&m_rootSignature)));

			const D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS = ShaderByteCode };
			ThrowIfFailed(deviceContext.Device->CreateComputePipelineState(&pipelineStateDesc, IID_PPV_ARGS(&m_pipelineState)));
			m_pipelineState->SetName(L"DenoisedComposition");
		}

		void Process(CommandList& commandList, const Constants& constants) {
			commandList->SetComputeRootSignature(m_rootSignature.Get());
			commandList->SetPipelineState(m_pipelineState.Get());

			commandList.SetState(*GPUBuffers.Camera, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
			commandList.SetState(*Textures.LinearDepth, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
			commandList.SetState(*Textures.BaseColorMetalness, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
			commandList.SetState(*Textures.NormalRoughness, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
			commandList.SetState(*Textures.DenoisedDiffuse, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
			commandList.SetState(*Textures.DenoisedSpecular, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
			commandList.SetState(*Textures.Radiance, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

			UINT i = 0;
			commandList->SetComputeRoot32BitConstants(i++, sizeof(constants) / 4, &constants, 0);
			commandList->SetComputeRootConstantBufferView(i++, GPUBuffers.Camera->GetNative()->GetGPUVirtualAddress());
			commandList->SetComputeRootDescriptorTable(i++, Textures.LinearDepth->GetSRVDescriptor());
			commandList->SetComputeRootDescriptorTable(i++, Textures.BaseColorMetalness->GetSRVDescriptor());
			commandList->SetComputeRootDescriptorTable(i++, Textures.NormalRoughness->GetSRVDescriptor());
			commandList->SetComputeRootDescriptorTable(i++, Textures.DenoisedDiffuse->GetSRVDescriptor());
			commandList->SetComputeRootDescriptorTable(i++, Textures.DenoisedSpecular->GetSRVDescriptor());
			commandList->SetComputeRootDescriptorTable(i++, Textures.Radiance->GetUAVDescriptor());

			commandList->Dispatch((constants.RenderSize.x + 15) / 16, (constants.RenderSize.y + 15) / 16, 1);
		}

	private:
		ComPtr<ID3D12RootSignature> m_rootSignature;
		ComPtr<ID3D12PipelineState> m_pipelineState;
	};
}
