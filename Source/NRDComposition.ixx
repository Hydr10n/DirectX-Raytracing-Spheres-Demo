module;

#include "directxtk12/DirectXHelpers.h"

#include "Shaders/NRDComposition.dxil.h"

export module PostProcessing.NRDComposition;

import CommandList;
import Denoiser;
import DeviceContext;
import ErrorHelpers;
import GPUBuffer;
import Texture;

using namespace DirectX;
using namespace ErrorHelpers;
using namespace Microsoft::WRL;
using namespace std;

export namespace PostProcessing {
	struct NRDComposition {
		struct Constants {
			XMUINT2 RenderSize;
			uint32_t Pack;
			Denoiser Denoiser;
			XMFLOAT4 ReBLURHitDistance;
		};

		struct {
			Texture
				* LinearDepth,
				* DiffuseAlbedo,
				* SpecularAlbedo,
				* NormalRoughness,
				* NoisyDiffuse,
				* NoisySpecular,
				* DenoisedDiffuse,
				* DenoisedSpecular,
				* Radiance;
		} Textures{};

		NRDComposition(const NRDComposition&) = delete;
		NRDComposition& operator=(const NRDComposition&) = delete;

		explicit NRDComposition(const DeviceContext& deviceContext) noexcept(false) {
			constexpr D3D12_SHADER_BYTECODE ShaderByteCode{ g_NRDComposition_dxil, size(g_NRDComposition_dxil) };

			ThrowIfFailed(deviceContext.Device->CreateRootSignature(0, ShaderByteCode.pShaderBytecode, ShaderByteCode.BytecodeLength, IID_PPV_ARGS(&m_rootSignature)));

			const D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS = ShaderByteCode };
			ThrowIfFailed(deviceContext.Device->CreateComputePipelineState(&pipelineStateDesc, IID_PPV_ARGS(&m_pipelineState)));
			m_pipelineState->SetName(L"NRDComposition");
		}

		void Process(CommandList& commandList, const Constants& constants) {
			commandList->SetComputeRootSignature(m_rootSignature.Get());
			commandList->SetPipelineState(m_pipelineState.Get());

			commandList.SetState(*Textures.LinearDepth, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
			commandList.SetState(*Textures.DiffuseAlbedo, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
			commandList.SetState(*Textures.SpecularAlbedo, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
			commandList.SetState(*Textures.NormalRoughness, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
			commandList.SetState(*Textures.DenoisedDiffuse, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
			commandList.SetState(*Textures.DenoisedSpecular, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
			commandList.SetState(*Textures.NoisyDiffuse, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			commandList.SetState(*Textures.NoisySpecular, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			commandList.SetState(*Textures.Radiance, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

			uint32_t i = 0;
			commandList->SetComputeRoot32BitConstants(i++, sizeof(constants) / 4, &constants, 0);
			commandList->SetComputeRootDescriptorTable(i++, Textures.LinearDepth->GetSRVDescriptor());
			commandList->SetComputeRootDescriptorTable(i++, Textures.DiffuseAlbedo->GetSRVDescriptor());
			commandList->SetComputeRootDescriptorTable(i++, Textures.SpecularAlbedo->GetSRVDescriptor());
			commandList->SetComputeRootDescriptorTable(i++, Textures.NormalRoughness->GetSRVDescriptor());
			commandList->SetComputeRootDescriptorTable(i++, Textures.DenoisedDiffuse->GetSRVDescriptor());
			commandList->SetComputeRootDescriptorTable(i++, Textures.DenoisedSpecular->GetSRVDescriptor());
			commandList->SetComputeRootDescriptorTable(i++, Textures.NoisyDiffuse->GetUAVDescriptor());
			commandList->SetComputeRootDescriptorTable(i++, Textures.NoisySpecular->GetUAVDescriptor());
			commandList->SetComputeRootDescriptorTable(i++, Textures.Radiance->GetUAVDescriptor());

			commandList->Dispatch((constants.RenderSize.x + 15) / 16, (constants.RenderSize.y + 15) / 16, 1);
		}

	private:
		ComPtr<ID3D12RootSignature> m_rootSignature;
		ComPtr<ID3D12PipelineState> m_pipelineState;
	};
}
