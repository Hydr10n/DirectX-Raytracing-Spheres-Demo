module;

#include <mutex>
#include <vector>

#include "directxtk12/DescriptorHeap.h"

export module DescriptorHeap;

import ErrorHelpers;

using namespace ErrorHelpers;
using namespace std;

export namespace DirectX {
	class DescriptorHeapEx;

	class Descriptor {
	public:
		~Descriptor();

		bool IsValid() const { return m_index != ~0u && m_count; }

		uint32_t GetIndex() const { return m_index; }
		operator uint32_t() const { return m_index; }

		uint32_t GetCount() const { return m_count; }

		D3D12_CPU_DESCRIPTOR_HANDLE GetCPUHandle() const;
		operator D3D12_CPU_DESCRIPTOR_HANDLE() const { return GetCPUHandle(); }

		D3D12_GPU_DESCRIPTOR_HANDLE GetGPUHandle() const;
		operator D3D12_GPU_DESCRIPTOR_HANDLE() const { return GetGPUHandle(); }

		const DescriptorHeapEx& GetHeap() const { return m_heap; }
		DescriptorHeapEx& GetHeap() { return m_heap; }

	private:
		friend DescriptorHeapEx;
		DescriptorHeapEx& m_heap;

		uint32_t m_index = ~0u, m_count{};

		Descriptor(DescriptorHeapEx& heap, uint32_t index, uint32_t count) : m_heap(heap), m_index(index), m_count(count) {}
	};

	class DescriptorHeapEx : public DescriptorHeap {
	public:
		DescriptorHeapEx(const DescriptorHeapEx&) = delete;
		DescriptorHeapEx& operator=(const DescriptorHeapEx&) = delete;

		DescriptorHeapEx(ID3D12DescriptorHeap* pExistingHeap) noexcept :
			DescriptorHeap(pExistingHeap),
			m_availableDescriptorCount(pExistingHeap->GetDesc().NumDescriptors),
			m_descriptors(m_availableDescriptorCount) {
		}

		DescriptorHeapEx(ID3D12Device* pDevice, const D3D12_DESCRIPTOR_HEAP_DESC& desc) noexcept(false) :
			DescriptorHeap(pDevice, &desc),
			m_availableDescriptorCount(desc.NumDescriptors),
			m_descriptors(m_availableDescriptorCount) {
		}

		DescriptorHeapEx(
			ID3D12Device* pDevice,
			uint32_t capacity,
			D3D12_DESCRIPTOR_HEAP_TYPE type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
			D3D12_DESCRIPTOR_HEAP_FLAGS flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
		) noexcept(false) :
			DescriptorHeap(pDevice, type, flags, capacity),
			m_availableDescriptorCount(capacity),
			m_descriptors(m_availableDescriptorCount) {
		}

		ID3D12DescriptorHeap* operator->() const noexcept(false) { return Heap(); }
		operator ID3D12DescriptorHeap* () const noexcept(false) { return Heap(); }

		// May be fragmented
		uint32_t GetAvailableDescriptorCount() const noexcept(false) { return m_availableDescriptorCount; }

		unique_ptr<Descriptor> Allocate(uint32_t count = 1) {
			if (!count) Throw<out_of_range>("Cannot allocate 0 descriptors");

			const scoped_lock lock(m_mutex);

			if (count <= m_availableDescriptorCount) {
				for (uint32_t i = 0; i < size(m_descriptors); i++) {
					if (!m_descriptors[i]) {
						auto found = true;
						for (uint32_t j = 1; j < count; j++) {
							if (m_descriptors[i + j]) {
								i += j;
								found = false;
								break;
							}
						}
						if (found) {
							for (uint32_t j = 0; j < count; j++) m_descriptors[i + j] = true;
							m_availableDescriptorCount -= count;
							return unique_ptr<Descriptor>(new Descriptor(*this, i, count));
						}
					}
				}
			}

			Throw<runtime_error>("Not enough available descriptors");
		}

		void Free(Descriptor& descriptor) {
			if (!descriptor.IsValid() || &descriptor.m_heap != this) return;

			const scoped_lock lock(m_mutex);

			if (const auto size = static_cast<uint32_t>(::size(m_descriptors)); descriptor.m_index < size) {
				const auto count = min(size, descriptor.m_count);
				for (uint32_t i = 0; i < count; i++) m_descriptors[descriptor.m_index + i] = false;
				descriptor.m_index = ~0u;
				descriptor.m_count = 0;
				m_availableDescriptorCount += count;
			}
		}

	private:
		mutable mutex m_mutex;
		atomic_uint32_t m_availableDescriptorCount{};
		vector<bool> m_descriptors;
	};

	Descriptor::~Descriptor() { m_heap.Free(*this); }
	D3D12_CPU_DESCRIPTOR_HANDLE Descriptor::GetCPUHandle() const { return m_heap.GetCpuHandle(m_index); }
	D3D12_GPU_DESCRIPTOR_HANDLE Descriptor::GetGPUHandle() const { return m_heap.GetGpuHandle(m_index); }
}
