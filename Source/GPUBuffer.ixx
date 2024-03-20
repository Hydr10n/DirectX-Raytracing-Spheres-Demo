module;

#include <ranges>

#include "directx/d3dx12.h"

#include "directxtk12/DescriptorHeap.h"
#include "directxtk12/DirectXHelpers.h"
#include "directxtk12/ResourceUploadBatch.h"

#include "FormatHelpers.h"

export module GPUBuffer;

import DescriptorHeap;
import ErrorHelpers;
import GPUResource;

using namespace DirectX::FormatHelpers;
using namespace ErrorHelpers;
using namespace Microsoft::WRL;
using namespace std;

namespace {
	auto GetDevice(ID3D12Resource* pResource) {
		ComPtr<ID3D12Device> device;
		ThrowIfFailed(pResource->GetDevice(IID_PPV_ARGS(&device)));
		return device;
	}

	void CreateSRV(ID3D12Resource* pResource, D3D12_CPU_DESCRIPTOR_HANDLE descriptor, DXGI_FORMAT format, UINT count, UINT stride = 0, D3D12_BUFFER_SRV_FLAGS flags = D3D12_BUFFER_SRV_FLAG_NONE) {
		const D3D12_SHADER_RESOURCE_VIEW_DESC desc{
			.Format = format,
			.ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
			.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
			.Buffer{
				.NumElements = count,
				.StructureByteStride = stride,
				.Flags = flags
			}
		};
		GetDevice(pResource)->CreateShaderResourceView(pResource, &desc, descriptor);
	}

	void CreateUAV(ID3D12Resource* pResource, D3D12_CPU_DESCRIPTOR_HANDLE descriptor, DXGI_FORMAT format, UINT count, UINT stride = 0, D3D12_BUFFER_UAV_FLAGS flags = D3D12_BUFFER_UAV_FLAG_NONE) {
		const D3D12_UNORDERED_ACCESS_VIEW_DESC desc{
			.Format = format,
			.ViewDimension = D3D12_UAV_DIMENSION_BUFFER,
			.Buffer{
				.NumElements = count,
				.StructureByteStride = stride,
				.Flags = flags
			}
		};
		GetDevice(pResource)->CreateUnorderedAccessView(pResource, nullptr, &desc, descriptor);
	}
}

#define DEFAULT_BUFFER_BASE GPUBuffer<T, D3D12_HEAP_TYPE_DEFAULT, Alignment>
#define MAPPABLE_BUFFER_BASE GPUBuffer<T, IsUpload ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_READBACK, Alignment>
#define MAPPABLE_BUFFER_INITIAL_STATE IsUpload ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST
#define DEFAULT_ALIGNMENT size_t Alignment = is_arithmetic_v<T> || is_enum_v<T> ? sizeof(T) : 2

#define DESCRIPTOR(descriptor) \
	m_descriptors.descriptor = { \
		.HeapIndex = index, \
		.CPUHandle = descriptorHeap.GetCpuHandle(index), \
		.GPUHandle = descriptorHeap.GetGpuHandle(index) \
	}

export namespace DirectX {
	void CreateCBV(ID3D12Resource* pResource, D3D12_CPU_DESCRIPTOR_HANDLE descriptor, UINT size = 0) {
		const D3D12_CONSTANT_BUFFER_VIEW_DESC desc{
			.BufferLocation = pResource->GetGPUVirtualAddress(),
			.SizeInBytes = size ? size : static_cast<UINT>(pResource->GetDesc().Width)
		};
		GetDevice(pResource)->CreateConstantBufferView(&desc, descriptor);
	}

	void CreateStructuredSRV(ID3D12Resource* pResource, D3D12_CPU_DESCRIPTOR_HANDLE descriptor, UINT stride, UINT count) {
		CreateSRV(pResource, descriptor, DXGI_FORMAT_UNKNOWN, count, stride);
	}

	void CreateRawSRV(ID3D12Resource* pResource, D3D12_CPU_DESCRIPTOR_HANDLE descriptor, UINT size = 0) {
		CreateSRV(pResource, descriptor, DXGI_FORMAT_R32_TYPELESS, ((size ? size : static_cast<UINT>(pResource->GetDesc().Width)) + 3) / 4, 0, D3D12_BUFFER_SRV_FLAG_RAW);
	}

	void CreateTypedSRV(ID3D12Resource* pResource, D3D12_CPU_DESCRIPTOR_HANDLE descriptor, DXGI_FORMAT format, UINT count) {
		const auto size = GetBits(format) / 8;
		CreateSRV(pResource, descriptor, format, (size * count + size - 1) / size);
	}

	void CreateStructuredUAV(ID3D12Resource* pResource, D3D12_CPU_DESCRIPTOR_HANDLE descriptor, UINT stride, UINT count) {
		CreateUAV(pResource, descriptor, DXGI_FORMAT_UNKNOWN, count, stride);
	}

	void CreateRawUAV(ID3D12Resource* pResource, D3D12_CPU_DESCRIPTOR_HANDLE descriptor, UINT size = 0) {
		CreateUAV(pResource, descriptor, DXGI_FORMAT_R32_TYPELESS, ((size ? size : static_cast<UINT>(pResource->GetDesc().Width)) + 3) / 4, 0, D3D12_BUFFER_UAV_FLAG_RAW);
	}

	void CreateTypedUAV(ID3D12Resource* pResource, D3D12_CPU_DESCRIPTOR_HANDLE descriptor, DXGI_FORMAT format, UINT count) {
		const auto size = GetBits(format) / 8;
		CreateUAV(pResource, descriptor, format, (size * count + size - 1) / size);
	}

	template <typename T, D3D12_HEAP_TYPE HeapType, DEFAULT_ALIGNMENT>
	class GPUBuffer : public GPUResource {
	public:
		static_assert(IsPowerOf2(Alignment));

		using ElementType = T;

		static constexpr size_t ElementSize = (sizeof(T) + Alignment - 1) & ~(Alignment - 1);

		GPUBuffer(GPUBuffer&& source) noexcept = default;
		GPUBuffer& operator=(GPUBuffer&& source) noexcept = default;

		GPUBuffer(
			ID3D12Device* pDevice,
			size_t capacity = 1,
			D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE
		) noexcept(false) : GPUResource(initialState), m_capacity(capacity), m_count(capacity) {
			const CD3DX12_HEAP_PROPERTIES heapProperties(HeapType);
			const auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(AlignUp(ElementSize * capacity, 16), flags);
			ThrowIfFailed(pDevice->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &resourceDesc, initialState, nullptr, IID_PPV_ARGS(&m_resource)));
		}

		size_t GetCapacity() const noexcept { return m_capacity; }
		size_t GetCount() const noexcept { return m_count; }

		const Descriptor& GetCBVDescriptor() const noexcept { return m_descriptors.CBV; }
		Descriptor& GetCBVDescriptor() noexcept { return m_descriptors.CBV; }

		const Descriptor& GetStructuredSRVDescriptor() const noexcept { return m_descriptors.SRV.Structured; }
		Descriptor& GetStructuredSRVDescriptor() noexcept { return m_descriptors.SRV.Structured; }

		const Descriptor& GetRawSRVDescriptor() const noexcept { return m_descriptors.SRV.Raw; }
		Descriptor& GetRawSRVDescriptor() noexcept { return m_descriptors.SRV.Raw; }

		const Descriptor& GetTypedSRVDescriptor() const noexcept { return m_descriptors.SRV.Typed; }
		Descriptor& GetTypedSRVDescriptor() noexcept { return m_descriptors.SRV.Typed; }

		const Descriptor& GetStructuredUAVDescriptor() const noexcept { return m_descriptors.UAV.Structured; }
		Descriptor& GetStructuredUAVDescriptor() noexcept { return m_descriptors.UAV.Structured; }

		const Descriptor& GetRawUAVDescriptor() const noexcept { return m_descriptors.UAV.Raw; }
		Descriptor& GetRawUAVDescriptor() noexcept { return m_descriptors.UAV.Raw; }

		const Descriptor& GetTypedUAVDescriptor() const noexcept { return m_descriptors.UAV.Typed; }
		Descriptor& GetTypedUAVDescriptor() noexcept { return m_descriptors.UAV.Typed; }

		void CreateCBV(const DescriptorHeap& descriptorHeap, UINT index) {
			DESCRIPTOR(CBV);
			::CreateCBV(m_resource.Get(), m_descriptors.CBV.CPUHandle, static_cast<UINT>(ElementSize * m_count));
		}

		void CreateStructuredSRV(const DescriptorHeap& descriptorHeap, UINT index) {
			DESCRIPTOR(SRV.Structured);
			::CreateStructuredSRV(m_resource.Get(), m_descriptors.SRV.Structured.CPUHandle, static_cast<UINT>(ElementSize), static_cast<UINT>(m_count));
		}

		void CreateRawSRV(const DescriptorHeap& descriptorHeap, UINT index) {
			DESCRIPTOR(SRV.Raw);
			::CreateRawSRV(m_resource.Get(), m_descriptors.SRV.Raw.CPUHandle, static_cast<UINT>(ElementSize * m_count));
		}

		void CreateTypedSRV(const DescriptorHeap& descriptorHeap, UINT index, DXGI_FORMAT format) {
			DESCRIPTOR(SRV.Typed);
			::CreateTypedSRV(m_resource.Get(), m_descriptors.SRV.Typed.CPUHandle, format, static_cast<UINT>(m_count));
		}

		void CreateStructuredUAV(const DescriptorHeap& descriptorHeap, UINT index) {
			DESCRIPTOR(UAV.Structured);
			::CreateStructuredUAV(m_resource.Get(), m_descriptors.UAV.Structured.CPUHandle, static_cast<UINT>(ElementSize), static_cast<UINT>(m_count));
		}

		void CreateRawUAV(const DescriptorHeap& descriptorHeap, UINT index) {
			DESCRIPTOR(UAV.Raw);
			::CreateRawUAV(m_resource.Get(), m_descriptors.UAV.Raw.CPUHandle, static_cast<UINT>(ElementSize * m_count));
		}

		void CreateTypedUAV(const DescriptorHeap& descriptorHeap, UINT index, DXGI_FORMAT format) {
			DESCRIPTOR(UAV.Typed);
			::CreateTypedUAV(m_resource.Get(), m_descriptors.UAV.Typed.CPUHandle, format, static_cast<UINT>(m_count));
		}

	protected:
		size_t m_capacity{}, m_count{};

		GPUBuffer(const GPUBuffer& source, D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON) noexcept(false) : GPUResource(initialState), m_capacity(source.m_capacity), m_count(source.m_count) {
			ComPtr<ID3D12Device> device;
			ThrowIfFailed(source.m_resource->GetDevice(IID_PPV_ARGS(&device)));
			const CD3DX12_HEAP_PROPERTIES heapProperties(HeapType);
			const auto resourceDesc = source.m_resource->GetDesc();
			ThrowIfFailed(device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &resourceDesc, initialState, nullptr, IID_PPV_ARGS(&m_resource)));
		}

	private:
		struct {
			Descriptor CBV;
			struct { Descriptor Structured, Raw, Typed; } SRV, UAV;
		} m_descriptors;
	};

	template <typename T, DEFAULT_ALIGNMENT>
	struct DefaultBuffer : DEFAULT_BUFFER_BASE {
		using DEFAULT_BUFFER_BASE::GPUBuffer;

		DefaultBuffer(
			ID3D12Device* pDevice,
			size_t capacity = 1,
			D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAGS additionalFlags = D3D12_RESOURCE_FLAG_NONE
		) noexcept(false) : DEFAULT_BUFFER_BASE(pDevice, capacity, initialState, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | additionalFlags) {}

		DefaultBuffer(
			ID3D12Device* pDevice, ResourceUploadBatch& resourceUploadBatch,
			span<const T> data,
			D3D12_RESOURCE_STATES afterState = D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAGS additionalFlags = D3D12_RESOURCE_FLAG_NONE
		) noexcept(false) : DEFAULT_BUFFER_BASE(pDevice, size(data), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | additionalFlags) {
			Upload(resourceUploadBatch, data);
			resourceUploadBatch.Transition(this->m_resource.Get(), this->m_state, afterState);
			this->m_state = afterState;
		}

		DefaultBuffer(const DefaultBuffer& source, ID3D12GraphicsCommandList* pCommandList) noexcept(false) : DEFAULT_BUFFER_BASE(source) {
			if (pCommandList != nullptr) {
				const ScopedBarrier scopedBarrier(pCommandList, { CD3DX12_RESOURCE_BARRIER::Transition(source.m_resource.Get(), source.m_state, D3D12_RESOURCE_STATE_COPY_SOURCE) });
				this->TransitionTo(pCommandList, D3D12_RESOURCE_STATE_COPY_DEST);
				pCommandList->CopyBufferRegion(this->m_resource.Get(), 0, source.m_resource.Get(), 0, this->ElementSize * source.m_count);
				this->TransitionTo(pCommandList, source.m_state);
			}
		}

		void Upload(ResourceUploadBatch& resourceUploadBatch, span<const T> data) {
			const auto count = size(data);
			if (count > this->m_capacity) {
				ComPtr<ID3D12Device> device;
				ThrowIfFailed(this->m_resource->GetDevice(IID_PPV_ARGS(&device)));
				*this = DefaultBuffer(device.Get(), count);
			}
			else this->m_count = count;
			D3D12_SUBRESOURCE_DATA subresourceData{ .RowPitch = static_cast<LONG_PTR>(this->ElementSize * count) };
			vector<uint8_t> newData;
			if (sizeof(T) % Alignment == 0) subresourceData.pData = ::data(data);
			else {
				newData = vector<uint8_t>(this->ElementSize * count);
				for (const auto i : views::iota(static_cast<size_t>(0), count)) *reinterpret_cast<T*>(::data(newData) + this->ElementSize * i) = data[i];
				subresourceData.pData = ::data(newData);
			}
			const auto resource = this->m_resource.Get();
			resourceUploadBatch.Transition(resource, this->m_state, D3D12_RESOURCE_STATE_COPY_DEST);
			resourceUploadBatch.Upload(resource, 0, &subresourceData, 1);
			resourceUploadBatch.Transition(resource, D3D12_RESOURCE_STATE_COPY_DEST, this->m_state);
		}
	};

	template <typename T, bool IsUpload = true, DEFAULT_ALIGNMENT>
	class MappableBuffer : public MAPPABLE_BUFFER_BASE {
	public:
		MappableBuffer(MappableBuffer&& source) noexcept = default;
		MappableBuffer& operator=(MappableBuffer&& source) noexcept = default;

		MappableBuffer(
			ID3D12Device* pDevice,
			size_t capacity = 1,
			D3D12_RESOURCE_STATES initialState = MAPPABLE_BUFFER_INITIAL_STATE, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE
		) noexcept(false) : MAPPABLE_BUFFER_BASE(pDevice, capacity, initialState, flags) {
			Map();
		}

		MappableBuffer(
			ID3D12Device* pDevice,
			span<const T> data,
			D3D12_RESOURCE_STATES initialState = MAPPABLE_BUFFER_INITIAL_STATE, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE
		) noexcept(false) : MAPPABLE_BUFFER_BASE(pDevice, size(data), initialState, flags) {
			Map();
			Upload(data);
		}

		MappableBuffer(const MappableBuffer& source) noexcept(false) : MAPPABLE_BUFFER_BASE(source, MAPPABLE_BUFFER_INITIAL_STATE) {
			Map();
			for (const auto i : views::iota(static_cast<size_t>(0), this->m_count)) (*this)[i] = source[i];
		}

		void Upload(span<const T> data) {
			const auto count = size(data);
			if (count > this->m_capacity) {
				ComPtr<ID3D12Device> device;
				ThrowIfFailed(this->m_resource->GetDevice(IID_PPV_ARGS(&device)));
				*this = MappableBuffer(device.Get(), count);
			}
			else this->m_count = count;
			if (sizeof(T) % Alignment == 0) ranges::copy(data, reinterpret_cast<T*>(m_data));
			else {
				for (const auto i : views::iota(static_cast<size_t>(0), count)) (*this)[i] = data[i];
			}
		}

		void Map() { if (m_data == nullptr) ThrowIfFailed(this->m_resource->Map(0, nullptr, reinterpret_cast<void**>(&m_data))); }

		void Unmap() {
			if (m_data != nullptr) {
				this->m_resource->Unmap(0, nullptr);
				m_data = nullptr;
			}
		}

		const T& operator[](size_t index) const { return *reinterpret_cast<const T*>(m_data + this->ElementSize * index); }
		T& operator[](size_t index) { return const_cast<T&>(as_const(*this)[index]); }

		const T& At(size_t index) const {
			if (index >= this->m_count) throw out_of_range("Index out of range");
			return (*this)[index];
		}
		T& At(size_t index) { return const_cast<T&>(as_const(*this).At(index)); }

	private:
		uint8_t* m_data{};
	};

	template <typename T, DEFAULT_ALIGNMENT>
	using UploadBuffer = MappableBuffer<T, true, Alignment>;

	template <typename T, DEFAULT_ALIGNMENT>
	using ReadBackBuffer = MappableBuffer<T, false, Alignment>;

	template <typename T>
	using ConstantBuffer = UploadBuffer<T, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT>;
}
