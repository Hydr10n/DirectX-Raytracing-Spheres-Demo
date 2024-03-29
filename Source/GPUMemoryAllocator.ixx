module;

#include <map>

#include <wrl.h>

#include "D3D12MemAlloc.h"

export module GPUMemoryAllocator;

import ErrorHelpers;

using namespace D3D12MA;
using namespace ErrorHelpers;
using namespace Microsoft::WRL;
using namespace std;

export namespace DirectX {
	class GPUMemoryAllocator {
	public:
		explicit GPUMemoryAllocator(const ALLOCATOR_DESC& desc) noexcept(false) : m_device(desc.pDevice) {
			ThrowIfFailed(CreateAllocator(&desc, &m_allocators[desc.pDevice]));
		}

		~GPUMemoryAllocator() { m_allocators.erase(m_device); }

		static Allocator* Get(ID3D12Device* pDevice) { return m_allocators.at(pDevice).Get(); }
		Allocator* Get() const noexcept { return Get(m_device); }
		Allocator* operator->() const noexcept { return Get(); }
		operator Allocator* () const noexcept { return Get(); }

	private:
		ID3D12Device* m_device;
		inline static map<ID3D12Device*, ComPtr<Allocator>> m_allocators;
	};
}
