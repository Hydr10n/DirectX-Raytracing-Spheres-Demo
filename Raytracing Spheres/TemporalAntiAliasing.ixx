module;

#include "pch.h"

#include "directxtk12/PostProcess.h"

#include "Shaders/TemporalAntiAliasing.hlsl.h"

export module DirectX.PostProcess.TemporalAntiAliasing;

using namespace DX;
using namespace std;

export namespace DirectX::PostProcess {
	struct TemporalAntiAliasing : IPostProcess {
		struct Constant { float Alpha = 0.2f, ColorBoxSigma = 1; } Constant;

		SIZE TextureSize{};

		struct { D3D12_GPU_DESCRIPTOR_HANDLE PreviousOutputSRV, CurrentOutputSRV, MotionVectorsSRV, FinalOutputUAV; } TextureDescriptors{};

		TemporalAntiAliasing(ID3D12Device* device) {
			const CD3DX12_SHADER_BYTECODE shaderByteCode(g_pTemporalAntiAliasing, size(g_pTemporalAntiAliasing));
			ThrowIfFailed(device->CreateRootSignature(0, shaderByteCode.pShaderBytecode, shaderByteCode.BytecodeLength, IID_PPV_ARGS(&m_rootSignature)));
			const D3D12_COMPUTE_PIPELINE_STATE_DESC computePipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS = shaderByteCode };
			ThrowIfFailed(device->CreateComputePipelineState(&computePipelineStateDesc, IID_PPV_ARGS(&m_pipelineStateObject)));
		}

		void Process(ID3D12GraphicsCommandList* commandList) override {
			commandList->SetComputeRootSignature(m_rootSignature.Get());
			commandList->SetComputeRootDescriptorTable(0, TextureDescriptors.PreviousOutputSRV);
			commandList->SetComputeRootDescriptorTable(1, TextureDescriptors.CurrentOutputSRV);
			commandList->SetComputeRootDescriptorTable(2, TextureDescriptors.MotionVectorsSRV);
			commandList->SetComputeRootDescriptorTable(3, TextureDescriptors.FinalOutputUAV);
			commandList->SetComputeRoot32BitConstants(4, 2, &Constant, 0);
			commandList->SetPipelineState(m_pipelineStateObject.Get());
			commandList->Dispatch(static_cast<UINT>((TextureSize.cx + 16) / 16), static_cast<UINT>((TextureSize.cy + 16) / 16), 1);
		}

	private:
		Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
		Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pipelineStateObject;
	};
}
