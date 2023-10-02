module;

#include "pch.h"

#include "directxtk12/PostProcess.h"

#include "Shaders/ChromaticAberration.hlsl.h"

export module DirectX.PostProcess.ChromaticAberration;

import DirectX.BufferHelpers;

using namespace DirectX::BufferHelpers;
using namespace DX;
using namespace Microsoft::WRL;
using namespace std;

export namespace DirectX::PostProcess {
	struct Data {
		XMFLOAT2 FocusUV{ 0.5f, 0.5f };
		XMFLOAT2 _;
		XMFLOAT3 Offsets{ 3e-3f, 3e-3f, -3e-3f };
		float _1;
	};

	struct ChromaticAberration : IPostProcess {
		struct { D3D12_GPU_DESCRIPTOR_HANDLE Input, Output; } Descriptors{};

		XMUINT2 RenderSize{};

		ChromaticAberration(ID3D12Device* device) noexcept(false) : m_data(device) {
			const CD3DX12_SHADER_BYTECODE shaderByteCode(g_ChromaticAberration, size(g_ChromaticAberration));
			ThrowIfFailed(device->CreateRootSignature(0, shaderByteCode.pShaderBytecode, shaderByteCode.BytecodeLength, IID_PPV_ARGS(&m_rootSignature)));
			const D3D12_COMPUTE_PIPELINE_STATE_DESC computePipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS = shaderByteCode };
			ThrowIfFailed(device->CreateComputePipelineState(&computePipelineStateDesc, IID_PPV_ARGS(&m_pipelineStateObject)));
			m_data.GetData() = Data();
		}

		const auto& GetData() const { return m_data.GetData(); }
		auto& GetData() { return m_data.GetData(); }

		void Process(ID3D12GraphicsCommandList* commandList) noexcept override {
			commandList->SetComputeRootSignature(m_rootSignature.Get());
			commandList->SetComputeRootDescriptorTable(0, Descriptors.Input);
			commandList->SetComputeRootDescriptorTable(1, Descriptors.Output);
			commandList->SetComputeRoot32BitConstants(2, 2, &RenderSize, 0);
			commandList->SetComputeRootConstantBufferView(3, m_data.GetResource()->GetGPUVirtualAddress());
			commandList->SetPipelineState(m_pipelineStateObject.Get());
			commandList->Dispatch((RenderSize.x + 15) / 16, (RenderSize.y + 15) / 16, 1);
		}

	private:
		ComPtr<ID3D12RootSignature> m_rootSignature;
		ComPtr<ID3D12PipelineState> m_pipelineStateObject;
		ConstantBuffer<Data> m_data;
	};
}
