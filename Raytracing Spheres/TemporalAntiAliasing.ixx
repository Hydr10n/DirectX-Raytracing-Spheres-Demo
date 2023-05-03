module;

#include "pch.h"

#include "directxtk12/PostProcess.h"

#include "Shaders/TemporalAntiAliasing.hlsl.h"

export module DirectX.PostProcess.TemporalAntiAliasing;

import DirectX.BufferHelpers;

using namespace DirectX::BufferHelpers;
using namespace DX;
using namespace Microsoft::WRL;
using namespace std;

export namespace DirectX::PostProcess {
	struct TemporalAntiAliasing : IPostProcess {
		SIZE TextureSize{};
		struct { D3D12_GPU_DESCRIPTOR_HANDLE MotionSRV, HistoryOutputSRV, CurrentOutputSRV, FinalOutputUAV; } TextureDescriptors{};

		struct Data {
			XMFLOAT3 CameraPosition;
			float _1;
			XMFLOAT3 CameraRightDirection;
			float _2;
			XMFLOAT3 CameraUpDirection;
			float _3;
			XMFLOAT3 CameraForwardDirection;
			float CameraNearZ;
			XMFLOAT4X4 PreviousWorldToProjection;
		};

		TemporalAntiAliasing(ID3D12Device* device) noexcept(false) : m_data(device) {
			const CD3DX12_SHADER_BYTECODE shaderByteCode(g_pTemporalAntiAliasing, size(g_pTemporalAntiAliasing));
			ThrowIfFailed(device->CreateRootSignature(0, shaderByteCode.pShaderBytecode, shaderByteCode.BytecodeLength, IID_PPV_ARGS(&m_rootSignature)));
			const D3D12_COMPUTE_PIPELINE_STATE_DESC computePipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS = shaderByteCode };
			ThrowIfFailed(device->CreateComputePipelineState(&computePipelineStateDesc, IID_PPV_ARGS(&m_pipelineStateObject)));
		}

		const auto& GetData() const { return m_data.GetData(); }
		auto& GetData() { return m_data.GetData(); }

		void Process(ID3D12GraphicsCommandList* commandList) noexcept override {
			commandList->SetComputeRootSignature(m_rootSignature.Get());
			commandList->SetComputeRootDescriptorTable(0, TextureDescriptors.MotionSRV);
			commandList->SetComputeRootDescriptorTable(1, TextureDescriptors.HistoryOutputSRV);
			commandList->SetComputeRootDescriptorTable(2, TextureDescriptors.CurrentOutputSRV);
			commandList->SetComputeRootDescriptorTable(3, TextureDescriptors.FinalOutputUAV);
			commandList->SetComputeRootConstantBufferView(4, m_data.GetResource()->GetGPUVirtualAddress());
			commandList->SetPipelineState(m_pipelineStateObject.Get());
			commandList->Dispatch(static_cast<UINT>((TextureSize.cx + 16) / 16), static_cast<UINT>((TextureSize.cy + 16) / 16), 1);
		}

	private:
		ComPtr<ID3D12RootSignature> m_rootSignature;
		ComPtr<ID3D12PipelineState> m_pipelineStateObject;
		ConstantBuffer<Data> m_data;
	};
};
