module;

#include "directx/d3dx12.h"

#include "directxtk12/PostProcess.h"

#include "Shaders/DenoisedComposition.dxil.h"

export module PostProcessing.DenoisedComposition;

import Camera;
import ErrorHelpers;
import GPUBuffer;
import NRD;

using namespace DirectX;
using namespace ErrorHelpers;
using namespace Microsoft::WRL;
using namespace std;

export namespace PostProcessing {
	struct DenoisedComposition : IPostProcess {
		struct {
			XMUINT2 RenderSize;
			NRDDenoiser NRDDenoiser;
		} Constants{};

		struct { shared_ptr<ConstantBuffer<Camera>> InCamera; } GPUBuffers;

		struct { D3D12_GPU_DESCRIPTOR_HANDLE InLinearDepth, InBaseColorMetalness, InEmissiveColor, InNormalRoughness, InDenoisedDiffuse, InDenoisedSpecular, OutColor; } GPUDescriptors{};

		DenoisedComposition(ID3D12Device* pDevice) noexcept(false) {
			constexpr D3D12_SHADER_BYTECODE ShaderByteCode{ g_DenoisedComposition_dxil, size(g_DenoisedComposition_dxil) };
			ThrowIfFailed(pDevice->CreateRootSignature(0, ShaderByteCode.pShaderBytecode, ShaderByteCode.BytecodeLength, IID_PPV_ARGS(&m_rootSignature)));
			const D3D12_COMPUTE_PIPELINE_STATE_DESC computePipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS = ShaderByteCode };
			ThrowIfFailed(pDevice->CreateComputePipelineState(&computePipelineStateDesc, IID_PPV_ARGS(&m_pipelineStateObject)));
		}

		void Process(ID3D12GraphicsCommandList* commandList) override {
			commandList->SetComputeRootSignature(m_rootSignature.Get());
			commandList->SetComputeRoot32BitConstants(0, 3, &Constants, 0);
			commandList->SetComputeRootConstantBufferView(1, GPUBuffers.InCamera->GetResource()->GetGPUVirtualAddress());
			commandList->SetComputeRootDescriptorTable(2, GPUDescriptors.InLinearDepth);
			commandList->SetComputeRootDescriptorTable(3, GPUDescriptors.InBaseColorMetalness);
			commandList->SetComputeRootDescriptorTable(4, GPUDescriptors.InEmissiveColor);
			commandList->SetComputeRootDescriptorTable(5, GPUDescriptors.InNormalRoughness);
			commandList->SetComputeRootDescriptorTable(6, GPUDescriptors.InDenoisedDiffuse);
			commandList->SetComputeRootDescriptorTable(7, GPUDescriptors.InDenoisedSpecular);
			commandList->SetComputeRootDescriptorTable(8, GPUDescriptors.OutColor);
			commandList->SetPipelineState(m_pipelineStateObject.Get());
			commandList->Dispatch((Constants.RenderSize.x + 15) / 16, (Constants.RenderSize.y + 15) / 16, 1);
		}

	private:
		ComPtr<ID3D12RootSignature> m_rootSignature;
		ComPtr<ID3D12PipelineState> m_pipelineStateObject;
	};
}
