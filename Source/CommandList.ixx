module;

#include "directx/d3dx12.h"

#include <wrl.h>

#include <future>

export module CommandList;

import ErrorHelpers;

using namespace ErrorHelpers;
using namespace Microsoft::WRL;
using namespace Microsoft::WRL::Wrappers;
using namespace std;

export namespace DirectX {
	template <derived_from<ID3D12GraphicsCommandList> T = ID3D12GraphicsCommandList>
	class CommandList {
	public:
		CommandList(const CommandList&) = delete;
		CommandList& operator=(const CommandList&) = delete;

		CommandList(ID3D12Device* pDevice, D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT) noexcept(false) {
			ThrowIfFailed(pDevice->CreateCommandAllocator(type, IID_PPV_ARGS(&m_commandAllocator)));
			ThrowIfFailed(pDevice->CreateCommandList(0, type, m_commandAllocator.Get(), nullptr, IID_PPV_ARGS(&m_commandList)));
			ThrowIfFailed(m_commandList->Close());

			ThrowIfFailed(pDevice->CreateFence(m_fenceValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
			m_fenceEvent.Attach(CreateEventEx(nullptr, nullptr, 0, EVENT_MODIFY_STATE | SYNCHRONIZE));
			ThrowIfFailed(static_cast<BOOL>(m_fenceEvent.IsValid()));
		}

		T* GetNative() const noexcept { return m_commandList.Get(); }
		T* operator->() const noexcept { return m_commandList.Get(); }

		void Begin() { ThrowIfFailed(m_commandList->Reset(m_commandAllocator.Get(), nullptr)); }

		future<void> End(ID3D12CommandQueue* pCommandQueue) {
			return async(launch::async, [=] {
				ThrowIfFailed(m_commandList->Close());
			pCommandQueue->ExecuteCommandLists(1, CommandListCast(m_commandList.GetAddressOf()));

			ThrowIfFailed(pCommandQueue->Signal(m_fence.Get(), ++m_fenceValue));
			if (m_fence->GetCompletedValue() < m_fenceValue) {
				ThrowIfFailed(m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent.Get()));
				ignore = WaitForSingleObject(m_fenceEvent.Get(), INFINITE);
			}
				});
		}

	private:
		ComPtr<ID3D12CommandAllocator> m_commandAllocator;
		ComPtr<T> m_commandList;

		UINT m_fenceValue{};
		ComPtr<ID3D12Fence> m_fence;
		Event m_fenceEvent;
	};
}
