module;

#include "pch.h"

#include "directxtk12/PostProcess.h"

#include "Shaders/DenoisedComposition.hlsl.h"

export module DirectX.PostProcess.DenoisedComposition;

import DirectX.BufferHelpers;

using namespace DirectX::BufferHelpers;
using namespace DX;
using namespace Microsoft::WRL;
using namespace std;

export namespace DirectX::PostProcess {
	struct DenoisedComposition : IPostProcess {
		struct { D3D12_GPU_DESCRIPTOR_HANDLE InDepth, InBaseColorMetalness, InNormalRoughness, InDenoisedDiffuse, InDenoisedSpecular, Output; } Descriptors{};

		XMUINT2 RenderSize{};

		struct Data {
			XMFLOAT3 CameraRightDirection;
			float _;
			XMFLOAT3 CameraUpDirection;
			float _1;
			XMFLOAT3 CameraForwardDirection;
			float _2;
			XMFLOAT2 CameraPixelJitter;
			XMFLOAT2 _3;
		};

		DenoisedComposition(ID3D12Device* device) noexcept(false) : m_data(device) {
			const CD3DX12_SHADER_BYTECODE shaderByteCode(g_pDenoisedComposition, size(g_pDenoisedComposition));
			ThrowIfFailed(device->CreateRootSignature(0, shaderByteCode.pShaderBytecode, shaderByteCode.BytecodeLength, IID_PPV_ARGS(&m_rootSignature)));
			const D3D12_COMPUTE_PIPELINE_STATE_DESC computePipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS = shaderByteCode };
			ThrowIfFailed(device->CreateComputePipelineState(&computePipelineStateDesc, IID_PPV_ARGS(&m_pipelineStateObject)));
		}

		const auto& GetData() const { return m_data.GetData(); }
		auto& GetData() { return m_data.GetData(); }

		void Process(ID3D12GraphicsCommandList* commandList) noexcept override {
			commandList->SetComputeRootSignature(m_rootSignature.Get());
			commandList->SetComputeRootDescriptorTable(0, Descriptors.InDepth);
			commandList->SetComputeRootDescriptorTable(1, Descriptors.InBaseColorMetalness);
			commandList->SetComputeRootDescriptorTable(2, Descriptors.InNormalRoughness);
			commandList->SetComputeRootDescriptorTable(3, Descriptors.InDenoisedDiffuse);
			commandList->SetComputeRootDescriptorTable(4, Descriptors.InDenoisedSpecular);
			commandList->SetComputeRootDescriptorTable(5, Descriptors.Output);
			commandList->SetComputeRoot32BitConstants(6, 2, &RenderSize, 0);
			commandList->SetComputeRootConstantBufferView(7, m_data.GetResource()->GetGPUVirtualAddress());
			commandList->SetPipelineState(m_pipelineStateObject.Get());
			commandList->Dispatch((RenderSize.x + 16) / 16, (RenderSize.y + 16) / 16, 1);
		}

	private:
		ComPtr<ID3D12RootSignature> m_rootSignature;
		ComPtr<ID3D12PipelineState> m_pipelineStateObject;
		ConstantBuffer<Data> m_data;
	};
}