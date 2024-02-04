module;

#include "directx/d3dx12.h"

#include <DirectXMath.h>

#include "Shaders/ChromaticAberration.dxil.h"

export module PostProcessing.ChromaticAberration;

import ErrorHelpers;

using namespace DirectX;
using namespace ErrorHelpers;
using namespace Microsoft::WRL;
using namespace std;

export namespace PostProcessing {
	struct ChromaticAberration {
		struct {
			XMUINT2 RenderSize{};
			XMFLOAT2 FocusUV{ 0.5f, 0.5f };
			XMFLOAT3 Offsets{ 3e-3f, 3e-3f, -3e-3f };
		} Constants;

		struct { D3D12_GPU_DESCRIPTOR_HANDLE InColor, OutColor; } GPUDescriptors{};

		explicit ChromaticAberration(ID3D12Device* pDevice) noexcept(false) {
			constexpr D3D12_SHADER_BYTECODE ShaderByteCode{ g_ChromaticAberration_dxil, size(g_ChromaticAberration_dxil) };
			ThrowIfFailed(pDevice->CreateRootSignature(0, ShaderByteCode.pShaderBytecode, ShaderByteCode.BytecodeLength, IID_PPV_ARGS(&m_rootSignature)));
			const D3D12_COMPUTE_PIPELINE_STATE_DESC computePipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS = ShaderByteCode };
			ThrowIfFailed(pDevice->CreateComputePipelineState(&computePipelineStateDesc, IID_PPV_ARGS(&m_pipelineStateObject)));
		}

		void Process(ID3D12GraphicsCommandList* pCommandList) {
			pCommandList->SetPipelineState(m_pipelineStateObject.Get());
			pCommandList->SetComputeRootSignature(m_rootSignature.Get());
			pCommandList->SetComputeRoot32BitConstants(0, sizeof(Constants) / 4, &Constants, 0);
			pCommandList->SetComputeRootDescriptorTable(1, GPUDescriptors.InColor);
			pCommandList->SetComputeRootDescriptorTable(2, GPUDescriptors.OutColor);
			pCommandList->Dispatch((Constants.RenderSize.x + 15) / 16, (Constants.RenderSize.y + 15) / 16, 1);
		}

	private:
		ComPtr<ID3D12RootSignature> m_rootSignature;
		ComPtr<ID3D12PipelineState> m_pipelineStateObject;
	};
}
