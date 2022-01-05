#pragma once

#include "d3dx12.h"

namespace RaytracingHelpers {
	inline HRESULT CreateBuffer(ID3D12Device* pDevice, UINT64 size, D3D12_RESOURCE_STATES initialState, ID3D12Resource** ppBuffer, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE, const D3D12_HEAP_PROPERTIES& heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT)) {
		const auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(size, flags);
		return pDevice->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &resourceDesc, initialState, nullptr, IID_PPV_ARGS(ppBuffer));
	}

	inline HRESULT CreateUploadBuffer(ID3D12Device* pDevice, UINT64 size, ID3D12Resource** ppBuffer, const void* pData = nullptr) {
		PVOID pMappedData;
		HRESULT ret;
		if (SUCCEEDED(ret = CreateBuffer(pDevice, size, D3D12_RESOURCE_STATE_GENERIC_READ, ppBuffer, D3D12_RESOURCE_FLAG_NONE, CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD)))
			&& pData != nullptr && SUCCEEDED(ret = (*ppBuffer)->Map(0, nullptr, &pMappedData))) {
			memcpy(pMappedData, pData, size);
			(*ppBuffer)->Unmap(0, nullptr);
		}
		return ret;
	}

	struct AccelerationStructureBuffers {
		Microsoft::WRL::ComPtr<ID3D12Resource> Scratch, Result, InstanceDesc;

		AccelerationStructureBuffers() = default;

		AccelerationStructureBuffers(ID3D12Device* pDevice, UINT64 scratchSize, UINT64 resultSize, UINT64 instanceDescsSize = 0) noexcept(false) {
			DX::ThrowIfFailed(CreateBuffer(pDevice, scratchSize, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &Scratch, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS));
			DX::ThrowIfFailed(CreateBuffer(pDevice, resultSize, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, &Result, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS));
			if (instanceDescsSize) DX::ThrowIfFailed(CreateUploadBuffer(pDevice, instanceDescsSize, &InstanceDesc));
		}
	};
}
