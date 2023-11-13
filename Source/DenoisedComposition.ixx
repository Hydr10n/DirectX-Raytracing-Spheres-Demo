module;

#include "pch.h"

#include "directxtk12/PostProcess.h"

#include "Shaders/DenoisedComposition.dxil.h"

export module DirectX.PostProcess.DenoisedComposition;

import DirectX.BufferHelpers;
import NRD;

using namespace DirectX::BufferHelpers;
using namespace DX;
using namespace Microsoft::WRL;
using namespace std;

export namespace DirectX::PostProcess {
	struct DenoisedComposition : IPostProcess {
		struct { D3D12_GPU_DESCRIPTOR_HANDLE InLinearDepth, InBaseColorMetalness, InEmissiveColor, InNormalRoughness, InDenoisedDiffuse, InDenoisedSpecular, OutColor; } Descriptors{};

		XMUINT2 RenderSize{};

		struct Data {
			NRDDenoiser NRDDenoiser;
			XMFLOAT3 CameraRightDirection;
			XMFLOAT3 CameraUpDirection;
			float _;
			XMFLOAT3 CameraForwardDirection;
			float _1;
			XMFLOAT2 CameraJitter;
			XMFLOAT2 _2;
		};

		DenoisedComposition(ID3D12Device* device) noexcept(false) : m_data(device) {
			const CD3DX12_SHADER_BYTECODE shaderByteCode(g_DenoisedComposition_dxil, size(g_DenoisedComposition_dxil));
			ThrowIfFailed(device->CreateRootSignature(0, shaderByteCode.pShaderBytecode, shaderByteCode.BytecodeLength, IID_PPV_ARGS(&m_rootSignature)));
			const D3D12_COMPUTE_PIPELINE_STATE_DESC computePipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS = shaderByteCode };
			ThrowIfFailed(device->CreateComputePipelineState(&computePipelineStateDesc, IID_PPV_ARGS(&m_pipelineStateObject)));
		}

		const auto& GetData() const noexcept { return m_data.GetData(); }
		auto& GetData() noexcept { return m_data.GetData(); }

		void Process(ID3D12GraphicsCommandList* commandList) noexcept override {
			commandList->SetComputeRootSignature(m_rootSignature.Get());
			commandList->SetComputeRootDescriptorTable(0, Descriptors.InLinearDepth);
			commandList->SetComputeRootDescriptorTable(1, Descriptors.InBaseColorMetalness);
			commandList->SetComputeRootDescriptorTable(2, Descriptors.InEmissiveColor);
			commandList->SetComputeRootDescriptorTable(3, Descriptors.InNormalRoughness);
			commandList->SetComputeRootDescriptorTable(4, Descriptors.InDenoisedDiffuse);
			commandList->SetComputeRootDescriptorTable(5, Descriptors.InDenoisedSpecular);
			commandList->SetComputeRootDescriptorTable(6, Descriptors.OutColor);
			commandList->SetComputeRoot32BitConstants(7, 2, &RenderSize, 0);
			commandList->SetComputeRootConstantBufferView(8, m_data.GetResource()->GetGPUVirtualAddress());
			commandList->SetPipelineState(m_pipelineStateObject.Get());
			commandList->Dispatch((RenderSize.x + 15) / 16, (RenderSize.y + 15) / 16, 1);
		}

	private:
		ComPtr<ID3D12RootSignature> m_rootSignature;
		ComPtr<ID3D12PipelineState> m_pipelineStateObject;
		ConstantBuffer<Data> m_data;
	};
}
