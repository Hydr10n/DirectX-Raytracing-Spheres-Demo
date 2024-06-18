module;

#include "directx/d3dx12.h"

#include "directxtk12/DirectXHelpers.h"

#include "D3D12MemAlloc.h"

export module GPUResource;

import ErrorHelpers;

using namespace D3D12MA;
using namespace Microsoft::WRL;

export namespace DirectX {
	class GPUResource {
	public:
		GPUResource(GPUResource&) = delete;
		GPUResource& operator=(const GPUResource&) = delete;

		GPUResource(GPUResource&& source) noexcept = default;
		GPUResource& operator=(GPUResource&& source) noexcept = default;

		GPUResource(ID3D12Resource* pResource, D3D12_RESOURCE_STATES state) : m_resource(pResource), m_state(state) {}

		virtual ~GPUResource() {}

		ID3D12Resource* GetNative() const noexcept { return m_resource.Get(); }
		ID3D12Resource* operator->() const noexcept { return m_resource.Get(); }
		operator ID3D12Resource* () const noexcept { return m_resource.Get(); }

		D3D12_RESOURCE_STATES GetState() const noexcept { return m_state; }
		void SetState(D3D12_RESOURCE_STATES state) noexcept { m_state = state; }

		D3D12_RESOURCE_BARRIER TransitionBarrier(D3D12_RESOURCE_STATES state) const {
			return CD3DX12_RESOURCE_BARRIER::Transition(m_resource.Get(), m_state, state);
		}

		void TransitionTo(ID3D12GraphicsCommandList* pCommandList, D3D12_RESOURCE_STATES state) {
			if (state != m_state) {
				TransitionResource(pCommandList, GetNative(), m_state, state);
				m_state = state;
			}
			else if (state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) InsertUAVBarrier(pCommandList);
		}

		void InsertUAVBarrier(ID3D12GraphicsCommandList* pCommandList) {
			const auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(m_resource.Get());
			pCommandList->ResourceBarrier(1, &barrier);
		}

	protected:
		ComPtr<Allocation> m_allocation;
		ComPtr<ID3D12Resource> m_resource;

		D3D12_RESOURCE_STATES m_state = D3D12_RESOURCE_STATE_COMMON;

		explicit GPUResource(D3D12_RESOURCE_STATES state) : m_state(state) {}
	};
}
