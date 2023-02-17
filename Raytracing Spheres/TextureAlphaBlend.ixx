module;

#include "pch.h"

#include "directxtk12/PostProcess.h"

#include "Shaders/TextureAlphaBlend.hlsl.h"

export module DirectX.PostProcess.TextureAlphaBlend;

using namespace DX;
using namespace Microsoft::WRL;
using namespace std;

export namespace DirectX::PostProcess {
	struct TextureAlphaBlend : IPostProcess {
		SIZE TextureSize{};
		struct { D3D12_GPU_DESCRIPTOR_HANDLE ForegroundSRV, BackgroundUAV; } TextureDescriptors{};

		TextureAlphaBlend(ID3D12Device* device) noexcept(false) {
			const CD3DX12_SHADER_BYTECODE shaderByteCode(g_pTextureAlphaBlend, size(g_pTextureAlphaBlend));
			ThrowIfFailed(device->CreateRootSignature(0, shaderByteCode.pShaderBytecode, shaderByteCode.BytecodeLength, IID_PPV_ARGS(&m_rootSignature)));
			const D3D12_COMPUTE_PIPELINE_STATE_DESC computePipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS = shaderByteCode };
			ThrowIfFailed(device->CreateComputePipelineState(&computePipelineStateDesc, IID_PPV_ARGS(&m_pipelineStateObject)));
		}

		void Process(ID3D12GraphicsCommandList* commandList) noexcept override {
			commandList->SetComputeRootSignature(m_rootSignature.Get());
			commandList->SetComputeRootDescriptorTable(0, TextureDescriptors.ForegroundSRV);
			commandList->SetComputeRootDescriptorTable(1, TextureDescriptors.BackgroundUAV);
			commandList->SetPipelineState(m_pipelineStateObject.Get());
			commandList->Dispatch(static_cast<UINT>((TextureSize.cx + 16) / 16), static_cast<UINT>((TextureSize.cy + 16) / 16), 1);
		}

	private:
		ComPtr<ID3D12RootSignature> m_rootSignature;
		ComPtr<ID3D12PipelineState> m_pipelineStateObject;
	};
};
