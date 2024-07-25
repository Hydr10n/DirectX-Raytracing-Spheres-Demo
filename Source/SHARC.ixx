module;

#include "directx/d3dx12.h"

#include "directxtk12/DirectXHelpers.h"

#include "Shaders/SHARC.dxil.h"

export module SHARC;

import Camera;
import ErrorHelpers;
import GPUBuffer;

using namespace DirectX;
using namespace ErrorHelpers;
using namespace Microsoft::WRL;
using namespace std;

export {
	struct SHARC {
		struct Constants {
			UINT AccumulationFrames = 10, MaxStaleFrames = 64;
			float SceneScale;
		};

		struct {
			unique_ptr<DefaultBuffer<UINT64>> HashEntries;
			unique_ptr<DefaultBuffer<UINT>> HashCopyOffset;
			unique_ptr<DefaultBuffer<XMUINT4>> PreviousVoxelData, VoxelData;

			ConstantBuffer<Camera>* Camera;
		} GPUBuffers{};

		SHARC(const SHARC&) = delete;
		SHARC& operator=(const SHARC&) = delete;

		explicit SHARC(ID3D12Device* pDevice) : m_device(pDevice) {
			constexpr D3D12_SHADER_BYTECODE ShaderByteCode{ g_SHARC_dxil, size(g_SHARC_dxil) };

			ThrowIfFailed(pDevice->CreateRootSignature(0, ShaderByteCode.pShaderBytecode, ShaderByteCode.BytecodeLength, IID_PPV_ARGS(&m_rootSignature)));

			const D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS = ShaderByteCode };
			ThrowIfFailed(pDevice->CreateComputePipelineState(&pipelineStateDesc, IID_PPV_ARGS(&m_pipelineState)));
			m_pipelineState->SetName(L"SHARC");
		}

		void Configure(UINT capacity = 1 << 22) {
			GPUBuffers.HashEntries = make_unique<DefaultBuffer<UINT64>>(m_device, capacity);
			GPUBuffers.HashCopyOffset = make_unique<DefaultBuffer<UINT>>(m_device, capacity);
			GPUBuffers.PreviousVoxelData = make_unique<DefaultBuffer<XMUINT4>>(m_device, capacity);
			GPUBuffers.VoxelData = make_unique<DefaultBuffer<XMUINT4>>(m_device, capacity);
		}

		void Process(ID3D12GraphicsCommandList* pCommandList, const Constants& constants) {
			pCommandList->SetComputeRootSignature(m_rootSignature.Get());
			pCommandList->SetPipelineState(m_pipelineState.Get());

			pCommandList->SetComputeRootConstantBufferView(1, GPUBuffers.Camera->GetNative()->GetGPUVirtualAddress());
			pCommandList->SetComputeRootUnorderedAccessView(2, GPUBuffers.HashEntries->GetNative()->GetGPUVirtualAddress());
			pCommandList->SetComputeRootUnorderedAccessView(3, GPUBuffers.HashCopyOffset->GetNative()->GetGPUVirtualAddress());
			pCommandList->SetComputeRootUnorderedAccessView(4, GPUBuffers.PreviousVoxelData->GetNative()->GetGPUVirtualAddress());
			pCommandList->SetComputeRootUnorderedAccessView(5, GPUBuffers.VoxelData->GetNative()->GetGPUVirtualAddress());

			const auto Dispatch = [&](bool isResolve) {
				{
					const auto barriers = {
						CD3DX12_RESOURCE_BARRIER::UAV(*GPUBuffers.HashEntries),
						CD3DX12_RESOURCE_BARRIER::UAV(*GPUBuffers.HashCopyOffset)
					};
					pCommandList->ResourceBarrier(static_cast<UINT>(size(barriers)), data(barriers));
				}

				if (isResolve) {
					const auto barriers = {
						CD3DX12_RESOURCE_BARRIER::UAV(*GPUBuffers.PreviousVoxelData),
						CD3DX12_RESOURCE_BARRIER::UAV(*GPUBuffers.VoxelData)
					};
					pCommandList->ResourceBarrier(static_cast<UINT>(size(barriers)), data(barriers));
				}

				const auto capacity = static_cast<UINT>(GPUBuffers.HashEntries->GetCount());

				const struct {
					BOOL IsResolve;
					UINT Capacity, AccumulationFrames, MaxStaleFrames;
					float SceneScale;
				} _constants{
					.IsResolve = isResolve,
					.Capacity = capacity,
					.AccumulationFrames = constants.AccumulationFrames,
					.MaxStaleFrames = constants.MaxStaleFrames,
					.SceneScale = constants.SceneScale
				};
				pCommandList->SetComputeRoot32BitConstants(0, sizeof(_constants) / 4, &_constants, 0);

				pCommandList->Dispatch((capacity + 255) / 256, 1, 1);
			};
			Dispatch(true);
			Dispatch(false);
		}

	private:
		ID3D12Device* m_device;

		ComPtr<ID3D12RootSignature> m_rootSignature;
		ComPtr<ID3D12PipelineState> m_pipelineState;
	};
}
