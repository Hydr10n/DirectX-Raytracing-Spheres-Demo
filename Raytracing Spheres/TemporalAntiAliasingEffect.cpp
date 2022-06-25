#include "pch.h"

#include "TemporalAntiAliasingEffect.h"

#include "Shaders/TemporalAntiAliasing.hlsl.h"

using namespace DirectX;
using namespace DX;

struct alignas(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT) Constant { float Alpha, ColorBoxSigma; };

TemporalAntiAliasingEffect::TemporalAntiAliasingEffect(ID3D12Device* device) : m_device(device) {
	const CD3DX12_SHADER_BYTECODE shaderByteCode(g_pTemporalAntiAliasing, ARRAYSIZE(g_pTemporalAntiAliasing));
	ThrowIfFailed(device->CreateRootSignature(0, shaderByteCode.pShaderBytecode, shaderByteCode.BytecodeLength, IID_PPV_ARGS(&m_rootSignature)));
	const D3D12_COMPUTE_PIPELINE_STATE_DESC computePipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS = shaderByteCode };
	ThrowIfFailed(device->CreateComputePipelineState(&computePipelineStateDesc, IID_PPV_ARGS(&m_pipelineStateObject)));
}

void TemporalAntiAliasingEffect::Apply(ID3D12GraphicsCommandList* commandList) {
	commandList->SetComputeRootSignature(m_rootSignature.Get());
	commandList->SetComputeRootDescriptorTable(0, TextureDescriptors.PreviousOutputSRV);
	commandList->SetComputeRootDescriptorTable(1, TextureDescriptors.CurrentOutputSRV);
	commandList->SetComputeRootDescriptorTable(2, TextureDescriptors.MotionVectorsSRV);
	commandList->SetComputeRootDescriptorTable(3, TextureDescriptors.FinalOutputUAV);
	commandList->SetComputeRoot32BitConstants(4, 2, &Constant, 0);
	commandList->SetPipelineState(m_pipelineStateObject.Get());
	commandList->Dispatch(static_cast<UINT>((TextureSize.cx + 15) / 16), static_cast<UINT>((TextureSize.cy + 15) / 16), 1);
}
