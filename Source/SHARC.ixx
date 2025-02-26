module;

#include "directx/d3dx12.h"

#include "directxtk12/DirectXHelpers.h"

#include "Shaders/SHARC.dxil.h"

export module SHARC;

import CommandList;
import DeviceContext;
import ErrorHelpers;
import GPUBuffer;

using namespace DirectX;
using namespace ErrorHelpers;
using namespace Microsoft::WRL;
using namespace std;

export {
	struct SHARC {
		struct Constants {
			uint32_t AccumulationFrames = 10, MaxStaleFrames = 64;
			float SceneScale = 50;
			bool IsAntiFireflyEnabled{};
		};

		struct {
			unique_ptr<GPUBuffer> HashEntries, HashCopyOffset, PreviousVoxelData, VoxelData;

			GPUBuffer* Camera;
		} GPUBuffers{};

		SHARC(const SHARC&) = delete;
		SHARC& operator=(const SHARC&) = delete;

		explicit SHARC(const DeviceContext& deviceContext) : m_deviceContext(deviceContext) {
			constexpr D3D12_SHADER_BYTECODE ShaderByteCode{ g_SHARC_dxil, size(g_SHARC_dxil) };

			ThrowIfFailed(m_deviceContext.Device->CreateRootSignature(0, ShaderByteCode.pShaderBytecode, ShaderByteCode.BytecodeLength, IID_PPV_ARGS(&m_rootSignature)));

			const D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS = ShaderByteCode };
			ThrowIfFailed(m_deviceContext.Device->CreateComputePipelineState(&pipelineStateDesc, IID_PPV_ARGS(&m_pipelineState)));
			m_pipelineState->SetName(L"SHARC");
		}

		void Configure(uint32_t capacity = 1 << 22) {
			const auto Create = [&]<typename T>(auto & buffer) {
				buffer = GPUBuffer::CreateDefault<T>(m_deviceContext, capacity);
				buffer->CreateUAV(BufferUAVType::Clear);
			};
			Create.operator() < uint64_t > (GPUBuffers.HashEntries);
			Create.operator() < uint32_t > (GPUBuffers.HashCopyOffset);
			Create.operator() < XMUINT4 > (GPUBuffers.PreviousVoxelData);
			Create.operator() < XMUINT4 > (GPUBuffers.VoxelData);
		}

		void Process(CommandList& commandList, const Constants& constants) {
			commandList->SetComputeRootSignature(m_rootSignature.Get());
			commandList->SetPipelineState(m_pipelineState.Get());

			commandList.SetState(*GPUBuffers.HashEntries, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			commandList.SetState(*GPUBuffers.HashCopyOffset, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			commandList.SetState(*GPUBuffers.PreviousVoxelData, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			commandList.SetState(*GPUBuffers.VoxelData, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

			commandList->SetComputeRootConstantBufferView(1, GPUBuffers.Camera->GetNative()->GetGPUVirtualAddress());
			commandList->SetComputeRootUnorderedAccessView(2, GPUBuffers.HashEntries->GetNative()->GetGPUVirtualAddress());
			commandList->SetComputeRootUnorderedAccessView(3, GPUBuffers.HashCopyOffset->GetNative()->GetGPUVirtualAddress());
			commandList->SetComputeRootUnorderedAccessView(4, GPUBuffers.PreviousVoxelData->GetNative()->GetGPUVirtualAddress());
			commandList->SetComputeRootUnorderedAccessView(5, GPUBuffers.VoxelData->GetNative()->GetGPUVirtualAddress());

			const auto Dispatch = [&](bool isResolve) {
				commandList.SetUAVBarrier(*GPUBuffers.HashEntries);
				commandList.SetUAVBarrier(*GPUBuffers.HashCopyOffset);

				if (isResolve) {
					commandList.SetUAVBarrier(*GPUBuffers.PreviousVoxelData);
					commandList.SetUAVBarrier(*GPUBuffers.VoxelData);
				}

				const struct {
					uint32_t IsResolve;
					uint32_t Capacity, AccumulationFrames, MaxStaleFrames;
					float SceneScale;
					uint32_t IsAntiFireflyEnabled;
				} _constants{
					.IsResolve = isResolve,
					.Capacity = static_cast<uint32_t>(GPUBuffers.HashEntries->GetCapacity()),
					.AccumulationFrames = constants.AccumulationFrames,
					.MaxStaleFrames = constants.MaxStaleFrames,
					.SceneScale = constants.SceneScale,
					.IsAntiFireflyEnabled = constants.IsAntiFireflyEnabled
				};
				commandList->SetComputeRoot32BitConstants(0, sizeof(_constants) / 4, &_constants, 0);

				commandList->Dispatch((_constants.Capacity + 255) / 256, 1, 1);
			};
			Dispatch(true);
			Dispatch(false);
		}

	private:
		const DeviceContext& m_deviceContext;

		ComPtr<ID3D12RootSignature> m_rootSignature;
		ComPtr<ID3D12PipelineState> m_pipelineState;
	};
}
