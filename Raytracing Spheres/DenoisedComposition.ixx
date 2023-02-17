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
		struct Data {
			XMFLOAT3 CameraPosition;
			float _;
			XMFLOAT3 CameraRightDirection;
			float _1;
			XMFLOAT3 CameraUpDirection;
			float _2;
			XMFLOAT3 CameraForwardDirection;
			float _3;
			XMFLOAT2 CameraPixelJitter;
			XMFLOAT2 _4;
		};

		SIZE TextureSize{};
		struct { D3D12_GPU_DESCRIPTOR_HANDLE NormalRoughnessSRV, ViewZSRV, BaseColorMetalnessSRV, DenoisedDiffuseSRV, DenoisedSpecularSRV, OutputUAV; } TextureDescriptors{};

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
			commandList->SetComputeRootDescriptorTable(0, TextureDescriptors.NormalRoughnessSRV);
			commandList->SetComputeRootDescriptorTable(1, TextureDescriptors.ViewZSRV);
			commandList->SetComputeRootDescriptorTable(2, TextureDescriptors.BaseColorMetalnessSRV);
			commandList->SetComputeRootDescriptorTable(3, TextureDescriptors.DenoisedDiffuseSRV);
			commandList->SetComputeRootDescriptorTable(4, TextureDescriptors.DenoisedSpecularSRV);
			commandList->SetComputeRootDescriptorTable(5, TextureDescriptors.OutputUAV);
			commandList->SetComputeRootConstantBufferView(6, m_data.GetResource()->GetGPUVirtualAddress());
			commandList->SetPipelineState(m_pipelineStateObject.Get());
			commandList->Dispatch(static_cast<UINT>((TextureSize.cx + 16) / 16), static_cast<UINT>((TextureSize.cy + 16) / 16), 1);
		}

	private:
		ComPtr<ID3D12RootSignature> m_rootSignature;
		ComPtr<ID3D12PipelineState> m_pipelineStateObject;
		ConstantBuffer<Data> m_data;
	};
};
