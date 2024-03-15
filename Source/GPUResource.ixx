module;

#include "directxtk12/DirectXHelpers.h"

export module GPUResource;

import ErrorHelpers;

using namespace Microsoft::WRL;

export namespace DirectX {
	class GPUResource {
	public:
		GPUResource(GPUResource&) = delete;
		GPUResource& operator=(const GPUResource&) = delete;

		GPUResource(GPUResource&& source) noexcept = default;
		GPUResource& operator=(GPUResource&& source) noexcept = default;

		virtual ~GPUResource() {}

		ID3D12Resource* GetResource() const noexcept { return m_resource.Get(); }

		D3D12_RESOURCE_STATES GetState() const noexcept { return m_state; }
		void SetState(D3D12_RESOURCE_STATES state) noexcept { m_state = state; }

		void TransitionTo(ID3D12GraphicsCommandList* pCommandList, D3D12_RESOURCE_STATES state) {
			TransitionResource(pCommandList, GetResource(), m_state, state);
			m_state = state;
		}

	protected:
		GPUResource(D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON) : m_state(state) {}

		ComPtr<ID3D12Resource> m_resource;

		D3D12_RESOURCE_STATES m_state{};
	};
}
