#include "pch.h"

#include "TemporalAntiAliasingEffect.h"

#include "Shaders/TemporalAntiAliasing.hlsl.h"

using namespace DirectX;
using namespace DX;

struct alignas(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT) Constant { float Alpha, ColorBoxSigma; };

TemporalAntiAliasingEffect::TemporalAntiAliasingEffect(ID3D12Device* device) : m_device(device) {
	const CD3DX12_SHADER_BYTECODE CS(g_pTemporalAntiAliasing, ARRAYSIZE(g_pTemporalAntiAliasing));
	ThrowIfFailed(device->CreateRootSignature(0, CS.pShaderBytecode, CS.BytecodeLength, IID_PPV_ARGS(&m_rootSignature)));
	const D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS = CS };
	ThrowIfFailed(device->CreateComputePipelineState(&pipelineStateDesc, IID_PPV_ARGS(&m_pipelineStateObject)));
}

void TemporalAntiAliasingEffect::Apply(ID3D12GraphicsCommandList* commandList) {
	commandList->SetComputeRootSignature(m_rootSignature.Get());
	commandList->SetComputeRootDescriptorTable(0, Textures.PreviousOutputSRV);
	commandList->SetComputeRootDescriptorTable(1, Textures.CurrentOutputSRV);
	commandList->SetComputeRootDescriptorTable(2, Textures.MotionVectorsSRV);
	commandList->SetComputeRootDescriptorTable(3, Textures.FinalOutputUAV);
	commandList->SetComputeRoot32BitConstants(4, 2, &Constants, 0);
	commandList->SetPipelineState(m_pipelineStateObject.Get());
	commandList->Dispatch(static_cast<UINT>((Textures.Size.cx + 15) / 16), static_cast<UINT>((Textures.Size.cy + 15) / 16), 1);
}
