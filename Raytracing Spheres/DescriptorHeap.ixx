module;

#include "directxtk12/DescriptorHeap.h"

export module DirectX.DescriptorHeap;

using namespace std;

export namespace DirectX {
	class DescriptorHeapEx : public DescriptorHeap {
	public:
		DescriptorHeapEx(ID3D12DescriptorHeap* pExistingHeap, UINT reserve = 0) noexcept(false) :
			DescriptorHeap(pExistingHeap), m_top(reserve) {
			if (reserve > 0 && m_top >= Count()) throw out_of_range("Descriptor heap reserve out of range");
		}

		DescriptorHeapEx(ID3D12Device* device, const D3D12_DESCRIPTOR_HEAP_DESC* pDesc, UINT reserve = 0) noexcept(false) :
			DescriptorHeap(device, pDesc), m_top(reserve) {
			if (reserve > 0 && m_top >= Count()) throw out_of_range("Descriptor heap reserve out of range");
		}

		DescriptorHeapEx(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, D3D12_DESCRIPTOR_HEAP_FLAGS flags, UINT capacity, UINT reserve = 0) noexcept(false) :
			DescriptorHeap(device, type, flags, capacity), m_top(reserve) {
			if (reserve > 0 && m_top >= Count()) throw out_of_range("Descriptor heap reserve out of range");
		}

		DescriptorHeapEx(ID3D12Device* device, UINT count, UINT reserve = 0) noexcept(false) :
			DescriptorHeapEx(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, count, reserve) {}

		DescriptorHeapEx(const DescriptorHeapEx&) = delete;
		DescriptorHeapEx& operator=(const DescriptorHeapEx&) = delete;

		DescriptorHeapEx(DescriptorHeapEx&&) = default;
		DescriptorHeapEx& operator=(DescriptorHeapEx&&) = default;

		UINT GetTop() const { return m_top; }

		UINT Allocate(UINT count = 1, UINT index = ~0u) {
			if (!count) throw invalid_argument("Cannot allocate 0 descriptors");
			if (index == ~0u) index = m_top;
			index += count;
			if (index > Count()) throw out_of_range("Descriptor heap allocation out of range");
			if (index > m_top) m_top = index;
			return index;
		}

		void AllocateTo(UINT index) {
			if (index == m_top) throw invalid_argument("Cannot allocate 0 descriptors");
			if (index < m_top || index > Count()) throw out_of_range("Descriptor heap allocation out of range");
			m_top = index;
		}

		UINT Free(UINT count = 1) {
			if (!count) throw invalid_argument("Cannot free 0 descriptors");
			const auto index = m_top - count;
			if (index > m_top) throw out_of_range("Descriptor heap free out of range");
			return m_top = index;
		}

		void FreeTo(UINT index) {
			if (index == m_top) throw invalid_argument("Cannot free 0 descriptors");
			if (index > m_top) throw out_of_range("Descriptor heap free out of range");
			m_top = index;
		}

	private:
		UINT m_top;
	};
}
