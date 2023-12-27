module;

#include "directx/d3dx12.h"

#include "directxtk12/DirectXHelpers.h"
#include "directxtk12/ResourceUploadBatch.h"

#include "FormatHelpers.h"

#include <ranges>
#include <stdexcept>

export module GPUBuffer;

import ErrorHelpers;

using namespace DirectX::FormatHelpers;
using namespace ErrorHelpers;
using namespace Microsoft::WRL;
using namespace std;

export namespace DirectX {
	template <typename T, D3D12_HEAP_TYPE Type, size_t Alignment = 2>
	class GPUBuffer {
	public:
		static_assert(IsPowerOf2(Alignment));

		using ItemType = T;

		static constexpr D3D12_HEAP_TYPE HeapType = Type;

		static constexpr size_t ItemSize = (sizeof(T) + Alignment - 1) & ~(Alignment - 1);

		GPUBuffer& operator=(const GPUBuffer&) = delete;

		GPUBuffer(
			ID3D12Device* pDevice,
			size_t capacity = 1,
			D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE
		) noexcept(false) : m_capacity(capacity), m_count(capacity), m_state(initialState) {
			const CD3DX12_HEAP_PROPERTIES heapProperties(Type);
			const auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(ItemSize * capacity, flags);
			ThrowIfFailed(pDevice->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &resourceDesc, initialState, nullptr, IID_PPV_ARGS(&m_resource)));
		}

		GPUBuffer(GPUBuffer&& source) noexcept { Swap(*this, source); }
		GPUBuffer&& operator=(GPUBuffer&& source) noexcept {
			Swap(source);
			return move(*this);
		}

		ID3D12Resource* GetResource() const noexcept { return m_resource.Get(); }

		size_t GetCapacity() const noexcept { return m_capacity; }
		size_t GetCount() const noexcept { return m_count; }

		D3D12_RESOURCE_STATES GetState() const noexcept { return m_state; }
		void TransitionTo(ID3D12GraphicsCommandList* pCommandList, D3D12_RESOURCE_STATES state) {
			TransitionResource(pCommandList, m_resource.Get(), m_state, state);
			m_state = state;
		}

		void CreateCBV(D3D12_CPU_DESCRIPTOR_HANDLE descriptor) const {
			ComPtr<ID3D12Device> device;
			ThrowIfFailed(m_resource->GetDevice(IID_PPV_ARGS(&device)));
			const D3D12_CONSTANT_BUFFER_VIEW_DESC constantBufferViewDesc{
				.BufferLocation = m_resource->GetGPUVirtualAddress(),
				.SizeInBytes = static_cast<UINT>(m_resource->GetDesc().Width)
			};
			device->CreateConstantBufferView(&constantBufferViewDesc, descriptor);
		}

		void CreateStructuredSRV(D3D12_CPU_DESCRIPTOR_HANDLE descriptor) const {
			ComPtr<ID3D12Device> device;
			ThrowIfFailed(m_resource->GetDevice(IID_PPV_ARGS(&device)));
			CreateBufferShaderResourceView(device.Get(), m_resource.Get(), descriptor, ItemSize);
		}

		void CreateRawSRV(D3D12_CPU_DESCRIPTOR_HANDLE descriptor) const {
			ComPtr<ID3D12Device> device;
			ThrowIfFailed(m_resource->GetDevice(IID_PPV_ARGS(&device)));
			const D3D12_SHADER_RESOURCE_VIEW_DESC shaderResourceViewDesc{
				.Format = DXGI_FORMAT_R32_TYPELESS,
				.ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
				.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
				.Buffer{
					.NumElements = static_cast<UINT>((ItemSize * m_count + 3) / 4),
					.Flags = D3D12_BUFFER_SRV_FLAG_RAW
				}
			};
			device->CreateShaderResourceView(m_resource.Get(), &shaderResourceViewDesc, descriptor);
		}

		void CreateTypedSRV(D3D12_CPU_DESCRIPTOR_HANDLE descriptor, DXGI_FORMAT format) const {
			const auto size = GetBits(format) / 8;
			if (ItemSize != size) throw invalid_argument("Format size doesn't match");
			ComPtr<ID3D12Device> device;
			ThrowIfFailed(m_resource->GetDevice(IID_PPV_ARGS(&device)));
			const D3D12_SHADER_RESOURCE_VIEW_DESC shaderResourceViewDesc{
				.Format = format,
				.ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
				.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
				.Buffer{
					.NumElements = static_cast<UINT>((ItemSize * m_count + size - 1) / size)
				}
			};
			device->CreateShaderResourceView(m_resource.Get(), &shaderResourceViewDesc, descriptor);
		}

		void CreateStructuredUAV(D3D12_CPU_DESCRIPTOR_HANDLE descriptor) const {
			ComPtr<ID3D12Device> device;
			ThrowIfFailed(m_resource->GetDevice(IID_PPV_ARGS(&device)));
			CreateBufferUnorderedAccessView(device.Get(), m_resource.Get(), descriptor, ItemSize);
		}

		void CreateRawUAV(D3D12_CPU_DESCRIPTOR_HANDLE descriptor) const {
			ComPtr<ID3D12Device> device;
			ThrowIfFailed(m_resource->GetDevice(IID_PPV_ARGS(&device)));
			const D3D12_UNORDERED_ACCESS_VIEW_DESC unorderedAccessViewDesc{
				.Format = DXGI_FORMAT_R32_TYPELESS,
				.ViewDimension = D3D12_UAV_DIMENSION_BUFFER,
				.Buffer{
					.NumElements = static_cast<UINT>((ItemSize * m_count + 3) / 4),
					.Flags = D3D12_BUFFER_UAV_FLAG_RAW
				}
			};
			device->CreateUnorderedAccessView(m_resource.Get(), nullptr, &unorderedAccessViewDesc, descriptor);
		}

		void CreateTypedUAV(D3D12_CPU_DESCRIPTOR_HANDLE descriptor, DXGI_FORMAT format) const {
			const auto size = GetBits(format) / 8;
			if (ItemSize != size) throw invalid_argument("Format size doesn't match");
			ComPtr<ID3D12Device> device;
			ThrowIfFailed(m_resource->GetDevice(IID_PPV_ARGS(&device)));
			const D3D12_UNORDERED_ACCESS_VIEW_DESC unorderedAccessViewDesc{
				.Format = format,
				.ViewDimension = D3D12_UAV_DIMENSION_BUFFER,
				.Buffer{
					.NumElements = static_cast<UINT>((ItemSize * m_count + size - 1) / size)
				}
			};
			device->CreateUnorderedAccessView(m_resource.Get(), nullptr, &unorderedAccessViewDesc, descriptor);
		}

	protected:
		size_t m_capacity{}, m_count{};
		D3D12_RESOURCE_STATES m_state{};
		ComPtr<ID3D12Resource> m_resource;

		GPUBuffer(const GPUBuffer& source) noexcept(false) : m_capacity(source.m_capacity), m_count(source.m_count), m_state(source.m_state) {
			ComPtr<ID3D12Device> device;
			ThrowIfFailed(source.m_resource->GetDevice(IID_PPV_ARGS(&device)));
			const CD3DX12_HEAP_PROPERTIES heapProperties(Type);
			const auto resourceDesc = source.m_resource->GetDesc();
			ThrowIfFailed(device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &resourceDesc, source.m_state, nullptr, IID_PPV_ARGS(&m_resource)));
		}

		void Swap(GPUBuffer& source) {
			swap(m_capacity, source.m_capacity);
			swap(m_count, source.m_count);
			swap(m_state, source.m_state);
			swap(m_resource, source.m_resource);
		}
	};

	template <typename T>
	struct DefaultBuffer : GPUBuffer<T, D3D12_HEAP_TYPE_DEFAULT> {
		using GPUBuffer<T, D3D12_HEAP_TYPE_DEFAULT>::GPUBuffer;

		DefaultBuffer(
			ID3D12Device* pDevice,
			size_t capacity = 1,
			D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAGS additionalFlags = D3D12_RESOURCE_FLAG_NONE
		) noexcept(false) : GPUBuffer<T, D3D12_HEAP_TYPE_DEFAULT>(pDevice, capacity, initialState, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | additionalFlags) {}

		DefaultBuffer(
			ID3D12Device* pDevice, ResourceUploadBatch& resourceUploadBatch,
			span<const T> data,
			D3D12_RESOURCE_STATES afterState = D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAGS additionalFlags = D3D12_RESOURCE_FLAG_NONE
		) noexcept(false) : GPUBuffer<T, D3D12_HEAP_TYPE_DEFAULT>(pDevice, size(data), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | additionalFlags) {
			Upload(resourceUploadBatch, data);
			resourceUploadBatch.Transition(this->m_resource.Get(), D3D12_RESOURCE_STATE_COMMON, afterState);
		}

		DefaultBuffer(const DefaultBuffer& source, ID3D12GraphicsCommandList* pCommandList) noexcept(false) : GPUBuffer<T, D3D12_HEAP_TYPE_DEFAULT>(source) {
			if (pCommandList != nullptr) {
				const ScopedBarrier scopedBarrier(
					pCommandList,
					{
						CD3DX12_RESOURCE_BARRIER::Transition(this->m_resource.Get(), this->m_state, D3D12_RESOURCE_STATE_COPY_DEST),
						CD3DX12_RESOURCE_BARRIER::Transition(source.m_resource.Get(), source.m_state, D3D12_RESOURCE_STATE_COPY_SOURCE)
					}
				);
				pCommandList->CopyBufferRegion(this->m_resource.Get(), 0, source.m_resource.Get(), 0, this->ItemSize * source.m_count);
			}
		}

		void Upload(ResourceUploadBatch& resourceUploadBatch, span<const T> data) {
			const auto state = this->m_state;
			const auto count = size(data);
			D3D12_SUBRESOURCE_DATA subresourceData{ .RowPitch = static_cast<LONG_PTR>(this->ItemSize * count) };
			if (count > this->m_capacity) {
				ComPtr<ID3D12Device> device;
				ThrowIfFailed(this->m_resource->GetDevice(IID_PPV_ARGS(&device)));
				*this = DefaultBuffer(device.Get(), count);
			}
			else this->m_count = count;
			vector<uint8_t> newData;
			if (sizeof(T) % 2 == 0) subresourceData.pData = ::data(data);
			else {
				newData = vector<uint8_t>(this->ItemSize * count);
				for (const auto i : views::iota(static_cast<size_t>(0), count)) *reinterpret_cast<T*>(::data(newData) + this->ItemSize * i) = data[i];
				subresourceData.pData = ::data(newData);
			}
			const auto resource = this->m_resource.Get();
			resourceUploadBatch.Transition(resource, state, D3D12_RESOURCE_STATE_COPY_DEST);
			resourceUploadBatch.Upload(resource, 0, &subresourceData, 1);
			resourceUploadBatch.Transition(resource, D3D12_RESOURCE_STATE_COPY_DEST, state);
		}
	};

	template <typename T, bool IsUpload = true, size_t Alignment = 2>
	class MappableBuffer : public GPUBuffer<T, IsUpload ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_READBACK, Alignment> {
	public:
		MappableBuffer(
			ID3D12Device* pDevice,
			size_t capacity = 1,
			D3D12_RESOURCE_STATES initialState = IsUpload ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE
		) noexcept(false) : GPUBuffer<T, MappableBuffer::HeapType, Alignment>(pDevice, capacity, initialState, flags) { Map(); }

		MappableBuffer(
			ID3D12Device* pDevice,
			span<const T> data,
			D3D12_RESOURCE_STATES initialState = IsUpload ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE
		) noexcept(false) : GPUBuffer<T, MappableBuffer::HeapType, Alignment>(pDevice, size(data), initialState, flags) {
			Map();
			for (const auto i : views::iota(static_cast<size_t>(0), this->m_count)) (*this)[i] = data[i];
		}

		MappableBuffer(const MappableBuffer& source) noexcept(false) : GPUBuffer<T, MappableBuffer::HeapType, Alignment>(source) {
			Map();
			for (const auto i : views::iota(static_cast<size_t>(0), this->m_count)) (*this)[i] = source[i];
		}

		MappableBuffer(MappableBuffer&& source) noexcept { Swap(source); }
		MappableBuffer&& operator=(MappableBuffer&& source) noexcept {
			Swap(source);
			return move(*this);
		}

		void Upload(span<const T> data) {
			if (const auto count = size(data); count > this->m_capacity) {
				ComPtr<ID3D12Device> device;
				ThrowIfFailed(this->m_resource->GetDevice(IID_PPV_ARGS(&device)));
				*this = MappableBuffer(device.Get(), data);
			}
			else {
				for (const auto i : views::iota(static_cast<size_t>(0), count)) (*this)[i] = data[i];
				this->m_count = count;
			}
		}

		void Map() { if (m_data == nullptr) ThrowIfFailed(this->m_resource->Map(0, nullptr, reinterpret_cast<void**>(&m_data))); }

		void Unmap() {
			if (m_data != nullptr) {
				this->m_resource->Unmap(0, nullptr);
				m_data = nullptr;
			}
		}

		const T& operator[](size_t index) const {
			if (index >= this->m_count) throw out_of_range("");
			return *reinterpret_cast<const T*>(m_data + this->ItemSize * index);
		}
		T& operator[](size_t index) { return const_cast<T&>(as_const(*this)[index]); }

		const T& GetData(size_t index = 0) const { return (*this)[index]; }
		T& GetData(size_t index = 0) { return (*this)[index]; }

	protected:
		uint8_t* m_data{};

		void Swap(MappableBuffer& source) {
			GPUBuffer<T, MappableBuffer::HeapType, Alignment>::Swap(source);
			swap(m_data, source.m_data);
		}
	};

	template <typename T, size_t Alignment = 2>
	using UploadBuffer = MappableBuffer<T, true, Alignment>;

	template <typename T, size_t Alignment = 2>
	using ReadBackBuffer = MappableBuffer<T, false, Alignment>;

	template <typename T>
	using ConstantBuffer = UploadBuffer<T, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT>;
}
