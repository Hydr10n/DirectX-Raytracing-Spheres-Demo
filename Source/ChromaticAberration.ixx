module;

#include "directx/d3dx12.h"

#include "directxtk12/PostProcess.h"

#include "Shaders/ChromaticAberration.dxil.h"

export module PostProcessing.ChromaticAberration;

import ErrorHelpers;

using namespace DirectX;
using namespace ErrorHelpers;
using namespace Microsoft::WRL;
using namespace std;

export namespace PostProcessing {
	struct ChromaticAberration : IPostProcess {
		struct {
			XMUINT2 RenderSize{};
			XMFLOAT2 FocusUV{ 0.5f, 0.5f };
			XMFLOAT3 Offsets{ 3e-3f, 3e-3f, -3e-3f };
		} Constants;

		struct { D3D12_GPU_DESCRIPTOR_HANDLE InColor, OutColor; } Descriptors{};

		ChromaticAberration(ID3D12Device* pDevice) noexcept(false) {
			constexpr D3D12_SHADER_BYTECODE shaderByteCode{ g_ChromaticAberration_dxil, size(g_ChromaticAberration_dxil) };
			ThrowIfFailed(pDevice->CreateRootSignature(0, shaderByteCode.pShaderBytecode, shaderByteCode.BytecodeLength, IID_PPV_ARGS(&m_rootSignature)));
			const D3D12_COMPUTE_PIPELINE_STATE_DESC computePipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS = shaderByteCode };
			ThrowIfFailed(pDevice->CreateComputePipelineState(&computePipelineStateDesc, IID_PPV_ARGS(&m_pipelineStateObject)));
		}

		void Process(ID3D12GraphicsCommandList* commandList) override {
			commandList->SetComputeRootSignature(m_rootSignature.Get());
			commandList->SetComputeRoot32BitConstants(0, 7, &Constants.RenderSize, 0);
			commandList->SetComputeRootDescriptorTable(1, Descriptors.InColor);
			commandList->SetComputeRootDescriptorTable(2, Descriptors.OutColor);
			commandList->SetPipelineState(m_pipelineStateObject.Get());
			commandList->Dispatch((Constants.RenderSize.x + 15) / 16, (Constants.RenderSize.y + 15) / 16, 1);
		}

	private:
		ComPtr<ID3D12RootSignature> m_rootSignature;
		ComPtr<ID3D12PipelineState> m_pipelineStateObject;
	};
}
