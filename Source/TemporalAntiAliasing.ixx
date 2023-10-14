module;

#include "pch.h"

#include "directxtk12/PostProcess.h"

#include "Shaders/TemporalAntiAliasing.dxil.h"

export module DirectX.PostProcess.TemporalAntiAliasing;

import DirectX.BufferHelpers;

using namespace DirectX::BufferHelpers;
using namespace DX;
using namespace Microsoft::WRL;
using namespace std;

export namespace DirectX::PostProcess {
	struct TemporalAntiAliasing : IPostProcess {
		struct { D3D12_GPU_DESCRIPTOR_HANDLE InHistoryOutput, InCurrentOutput, InMotionVectors, OutFinalOutput; } Descriptors{};

		XMUINT2 RenderSize{};

		struct Data {
			BOOL Reset;
			XMFLOAT3 CameraPosition;
			XMFLOAT3 CameraRightDirection;
			float _;
			XMFLOAT3 CameraUpDirection;
			float _1;
			XMFLOAT3 CameraForwardDirection;
			float _2;
			XMFLOAT4X4 CameraPreviousWorldToProjection;
		};

		TemporalAntiAliasing(ID3D12Device* device) noexcept(false) : m_data(device) {
			const CD3DX12_SHADER_BYTECODE shaderByteCode(g_TemporalAntiAliasing_dxil, size(g_TemporalAntiAliasing_dxil));
			ThrowIfFailed(device->CreateRootSignature(0, shaderByteCode.pShaderBytecode, shaderByteCode.BytecodeLength, IID_PPV_ARGS(&m_rootSignature)));
			const D3D12_COMPUTE_PIPELINE_STATE_DESC computePipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS = shaderByteCode };
			ThrowIfFailed(device->CreateComputePipelineState(&computePipelineStateDesc, IID_PPV_ARGS(&m_pipelineStateObject)));
		}

		const auto& GetData() const noexcept { return m_data.GetData(); }
		auto& GetData() noexcept { return m_data.GetData(); }

		void Process(ID3D12GraphicsCommandList* commandList) noexcept override {
			commandList->SetComputeRootSignature(m_rootSignature.Get());
			commandList->SetComputeRootDescriptorTable(0, Descriptors.InHistoryOutput);
			commandList->SetComputeRootDescriptorTable(1, Descriptors.InCurrentOutput);
			commandList->SetComputeRootDescriptorTable(2, Descriptors.InMotionVectors);
			commandList->SetComputeRootDescriptorTable(3, Descriptors.OutFinalOutput);
			commandList->SetComputeRoot32BitConstants(4, 2, &RenderSize, 0);
			commandList->SetComputeRootConstantBufferView(5, m_data.GetResource()->GetGPUVirtualAddress());
			commandList->SetPipelineState(m_pipelineStateObject.Get());
			commandList->Dispatch((RenderSize.x + 15) / 16, (RenderSize.y + 15) / 16, 1);
		}

	private:
		ComPtr<ID3D12RootSignature> m_rootSignature;
		ComPtr<ID3D12PipelineState> m_pipelineStateObject;
		ConstantBuffer<Data> m_data;
	};
}
