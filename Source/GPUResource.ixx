module;

#include <wrl.h>

#include "D3D12MemAlloc.h"

export module GPUResource;

import DeviceContext;

using namespace D3D12MA;
using namespace Microsoft::WRL;

export namespace DirectX {
	class GPUResource {
	public:
		GPUResource(GPUResource&) = delete;
		GPUResource& operator=(const GPUResource&) = delete;

		GPUResource(
			const DeviceContext& deviceContext,
			ID3D12Resource* pResource,
			D3D12_RESOURCE_STATES initialState, bool keepInitialState
		) : GPUResource(deviceContext, initialState, keepInitialState) {
			m_resource = pResource;
		}

		virtual ~GPUResource() {}

		ID3D12Resource* GetNative() const noexcept { return m_allocation ? m_allocation->GetResource() : m_resource.Get(); }
		ID3D12Resource* operator->() const noexcept { return GetNative(); }
		operator ID3D12Resource* () const noexcept { return GetNative(); }

		const DeviceContext& GetDeviceContext() const noexcept { return m_deviceContext; }

		bool IsOwner() const noexcept { return m_allocation; }

		D3D12_RESOURCE_STATES GetInitialState() const noexcept { return m_initialState; }
		bool KeepInitialState() const noexcept { return m_keepInitialState; }

		D3D12_RESOURCE_STATES GetState() const noexcept { return m_state; }
		void SetState(D3D12_RESOURCE_STATES state) noexcept { m_state = state; }

	protected:
		const DeviceContext& m_deviceContext;

		ComPtr<Allocation> m_allocation;
		ComPtr<ID3D12Resource> m_resource;

		D3D12_RESOURCE_STATES m_state;

		explicit GPUResource(const DeviceContext& deviceContext, D3D12_RESOURCE_STATES initialState, bool keepInitialState) :
			m_deviceContext(deviceContext),
			m_state(initialState),
			m_initialState(initialState), m_keepInitialState(keepInitialState) {}

	private:
		D3D12_RESOURCE_STATES m_initialState;
		bool m_keepInitialState;
	};
}
