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
	struct SHARCBuffers {
		DefaultBuffer<UINT64> HashEntries;
		DefaultBuffer<UINT> HashCopyOffset;
		DefaultBuffer<XMUINT4> PreviousVoxelData, VoxelData;

		explicit SHARCBuffers(ID3D12Device* pDevice, UINT capacity = 1 << 22) :
			HashEntries(pDevice, capacity),
			HashCopyOffset(pDevice, capacity),
			PreviousVoxelData(pDevice, capacity), VoxelData(pDevice, capacity) {}
	};

	struct SHARC {
		struct Constants { float SceneScale; };

		struct { ConstantBuffer<Camera>* Camera; } GPUBuffers{};

		SHARC(const SHARC&) = delete;
		SHARC& operator=(const SHARC&) = delete;

		explicit SHARC(ID3D12Device* pDevice) {
			constexpr D3D12_SHADER_BYTECODE ShaderByteCode{ g_SHARC_dxil, size(g_SHARC_dxil) };

			ThrowIfFailed(pDevice->CreateRootSignature(0, ShaderByteCode.pShaderBytecode, ShaderByteCode.BytecodeLength, IID_PPV_ARGS(&m_rootSignature)));

			const D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS = ShaderByteCode };
			ThrowIfFailed(pDevice->CreateComputePipelineState(&pipelineStateDesc, IID_PPV_ARGS(&m_pipelineState)));
			m_pipelineState->SetName(L"SHARC");
		}

		void Process(ID3D12GraphicsCommandList* pCommandList, const SHARCBuffers& buffers, Constants constants) {
			pCommandList->SetComputeRootSignature(m_rootSignature.Get());
			pCommandList->SetPipelineState(m_pipelineState.Get());

			pCommandList->SetComputeRootConstantBufferView(1, GPUBuffers.Camera->GetNative()->GetGPUVirtualAddress());
			pCommandList->SetComputeRootUnorderedAccessView(2, buffers.HashEntries->GetGPUVirtualAddress());
			pCommandList->SetComputeRootUnorderedAccessView(3, buffers.HashCopyOffset->GetGPUVirtualAddress());
			pCommandList->SetComputeRootUnorderedAccessView(4, buffers.PreviousVoxelData->GetGPUVirtualAddress());
			pCommandList->SetComputeRootUnorderedAccessView(5, buffers.VoxelData->GetGPUVirtualAddress());

			const auto Dispatch = [&](bool isResolve) {
				{
					const auto barriers = {
						CD3DX12_RESOURCE_BARRIER::UAV(buffers.HashEntries),
						CD3DX12_RESOURCE_BARRIER::UAV(buffers.HashCopyOffset)
					};
					pCommandList->ResourceBarrier(static_cast<UINT>(size(barriers)), data(barriers));
				}

				if (isResolve) {
					const auto barriers = {
						CD3DX12_RESOURCE_BARRIER::UAV(buffers.PreviousVoxelData),
						CD3DX12_RESOURCE_BARRIER::UAV(buffers.VoxelData)
					};
					pCommandList->ResourceBarrier(static_cast<UINT>(size(barriers)), data(barriers));
				}

				const auto capacity = static_cast<UINT>(buffers.HashEntries.GetCount());

				const struct {
					BOOL IsResolve;
					UINT Capacity;
					float SceneScale;
				} _constants{ .IsResolve = isResolve, .Capacity = capacity, .SceneScale = constants.SceneScale };
				pCommandList->SetComputeRoot32BitConstants(0, sizeof(_constants) / 4, &_constants, 0);

				pCommandList->Dispatch((capacity + 255) / 256, 1, 1);
			};
			Dispatch(true);
			Dispatch(false);
		}

	private:
		ComPtr<ID3D12RootSignature> m_rootSignature;
		ComPtr<ID3D12PipelineState> m_pipelineState;
	};
}
