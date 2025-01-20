module;

#include "directxtk12/DirectXHelpers.h"

#include "Shaders/GBufferGeneration.dxil.h"

export module GBufferGeneration;

import CommandList;
import DeviceContext;
import ErrorHelpers;
import GPUBuffer;
import Texture;

using namespace DirectX;
using namespace ErrorHelpers;
using namespace Microsoft::WRL;
using namespace std;

#define SET(Name) \
	commandList.SetState(*Textures.Name, D3D12_RESOURCE_STATE_UNORDERED_ACCESS); \
	commandList->SetComputeRootDescriptorTable(i++, Textures.Name->GetUAVDescriptor());

#define SET1(Name) if (constants.Flags & Flags::Name) { SET(Name); } else i++;
#define SET2(Name) if (constants.Flags & Flags::Material) { SET(Name); } else i++;

export struct GBufferGeneration {
	struct Flags {
		enum {
			Radiance = 0x1,
			Position = 0x2,
			LinearDepth = 0x4,
			NormalizedDepth = 0x8,
			MotionVector = 0x10,
			FlatNormal = 0x20,
			Normals = 0x40,
			NormalRoughness = 0x80,
			Geometry = Position | LinearDepth | NormalizedDepth | MotionVector | FlatNormal | Normals | NormalRoughness,
			Material = Radiance | NormalRoughness | 0x100
		};
	};

	struct Constants {
		XMUINT2 RenderSize{};
		UINT Flags{};
	};

	struct { GPUBuffer* SceneData, * Camera, * InstanceData, * ObjectData; } GPUBuffers{};

	struct {
		Texture
			* Radiance,
			* Position,
			* LinearDepth,
			* NormalizedDepth,
			* MotionVector,
			* BaseColorMetalness,
			* FlatNormal,
			* Normals,
			* Roughness,
			* NormalRoughness,
			* Transmission,
			* IOR;
	} Textures{};

	explicit GBufferGeneration(const DeviceContext& deviceContext) noexcept(false) {
		constexpr D3D12_SHADER_BYTECODE ShaderByteCode{ g_GBufferGeneration_dxil, size(g_GBufferGeneration_dxil) };

		ThrowIfFailed(deviceContext.Device->CreateRootSignature(0, ShaderByteCode.pShaderBytecode, ShaderByteCode.BytecodeLength, IID_PPV_ARGS(&m_rootSignature)));

		const D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS = ShaderByteCode };
		ThrowIfFailed(deviceContext.Device->CreateComputePipelineState(&pipelineStateDesc, IID_PPV_ARGS(&m_pipelineState)));
		m_pipelineState->SetName(L"GBufferGeneration");
	}

	void Render(CommandList& commandList, D3D12_GPU_VIRTUAL_ADDRESS topLevelAccelerationStructure, const Constants& constants) {
		commandList->SetComputeRootSignature(m_rootSignature.Get());
		commandList->SetPipelineState(m_pipelineState.Get());

		commandList.SetState(*GPUBuffers.SceneData, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
		commandList.SetState(*GPUBuffers.Camera, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
		if (GPUBuffers.InstanceData) commandList.SetState(*GPUBuffers.InstanceData, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		if (GPUBuffers.ObjectData) commandList.SetState(*GPUBuffers.ObjectData, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		UINT i = 0;
		commandList->SetComputeRootShaderResourceView(i++, topLevelAccelerationStructure);
		commandList->SetComputeRoot32BitConstants(i++, sizeof(constants) / 4, &constants, 0);
		commandList->SetComputeRootConstantBufferView(i++, GPUBuffers.SceneData->GetNative()->GetGPUVirtualAddress());
		commandList->SetComputeRootConstantBufferView(i++, GPUBuffers.Camera->GetNative()->GetGPUVirtualAddress());
		if (GPUBuffers.InstanceData) commandList->SetComputeRootShaderResourceView(i, GPUBuffers.InstanceData->GetNative()->GetGPUVirtualAddress());
		i++;
		if (GPUBuffers.ObjectData) commandList->SetComputeRootShaderResourceView(i, GPUBuffers.ObjectData->GetNative()->GetGPUVirtualAddress());
		i++;
		SET1(Radiance);
		SET1(Position);
		SET1(LinearDepth);
		SET1(NormalizedDepth);
		SET1(MotionVector);
		SET2(BaseColorMetalness);
		SET1(FlatNormal);
		SET1(Normals);
		SET2(Roughness);
		SET1(NormalRoughness);
		SET2(Transmission);
		SET2(IOR);

		commandList->Dispatch((constants.RenderSize.x + 15) / 16, (constants.RenderSize.y + 15) / 16, 1);
	}

private:
	ComPtr<ID3D12RootSignature> m_rootSignature;
	ComPtr<ID3D12PipelineState> m_pipelineState;
};
