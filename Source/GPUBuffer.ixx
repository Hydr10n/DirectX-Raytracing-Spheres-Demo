module;

#include "directx/d3dx12.h"

#include "directxtk12/BufferHelpers.h"
#include "directxtk12/DirectXHelpers.h"

#include <ranges>
#include <stdexcept>

export module GPUBuffer;

import ErrorHelpers;

using namespace ErrorHelpers;
using namespace Microsoft::WRL;
using namespace std;

export namespace DirectX {
	template <typename T, D3D12_HEAP_TYPE Type = D3D12_HEAP_TYPE_DEFAULT, size_t Alignment = 2>
	class GPUBuffer {
	public:
		static_assert(IsPowerOf2(Alignment));

		using ItemType = T;

		static constexpr D3D12_HEAP_TYPE HeapType = Type;

		static constexpr size_t ItemSize = (sizeof(T) + Alignment - 1) & ~(Alignment - 1);

		GPUBuffer(const GPUBuffer&) = delete;
		GPUBuffer& operator=(const GPUBuffer&) = delete;

		GPUBuffer(
			ID3D12Device* pDevice,
			size_t capacity = 1,
			D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE
		) noexcept(false) : m_capacity(capacity), m_state(initialState) {
			const CD3DX12_HEAP_PROPERTIES heapProperties(Type);
			const auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(ItemSize * capacity, flags);
			ThrowIfFailed(pDevice->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &resourceDesc, initialState, nullptr, IID_PPV_ARGS(&m_resource)));
		}

		GPUBuffer(const GPUBuffer& source, ID3D12GraphicsCommandList* commandList) noexcept(false) : m_capacity(source.m_capacity), m_state(source.m_state) {
			ComPtr<ID3D12Device> device;
			ThrowIfFailed(source.m_resource->GetDevice(IID_PPV_ARGS(&device)));
			const CD3DX12_HEAP_PROPERTIES heapProperties(Type);
			const auto resourceDesc = source.m_resource->GetDesc();
			ThrowIfFailed(device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &resourceDesc, commandList == nullptr ? source.m_state : D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_resource)));
			if (commandList != nullptr) {
				const ScopedBarrier scopedBarrier(commandList, { CD3DX12_RESOURCE_BARRIER::Transition(source.m_resource.Get(), source.m_state, D3D12_RESOURCE_STATE_COPY_SOURCE) });
				commandList->CopyResource(m_resource.Get(), source.m_resource.Get());
				TransitionResource(commandList, m_resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, source.m_state);
			}
		}

		GPUBuffer(GPUBuffer&& source) noexcept { Swap(*this, source); }
		GPUBuffer&& operator=(GPUBuffer&& source) noexcept {
			Swap(source);
			return move(*this);
		}

		ID3D12Resource* GetResource() const noexcept { return m_resource.Get(); }

		size_t GetCapacity() const noexcept { return m_capacity; }

		D3D12_RESOURCE_STATES GetState() const noexcept { return m_state; }
		void TransitionTo(ID3D12GraphicsCommandList* commandList, D3D12_RESOURCE_STATES state) {
			TransitionResource(commandList, m_resource.Get(), m_state, state);
			m_state = state;
		}

	protected:
		size_t m_capacity{};
		D3D12_RESOURCE_STATES m_state{};
		ComPtr<ID3D12Resource> m_resource;

		GPUBuffer() = default;

		void Swap(GPUBuffer& source) {
			swap(m_capacity, source.m_capacity);
			swap(m_state, source.m_state);
			swap(m_resource, source.m_resource);
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
			for (const auto i : views::iota(static_cast<size_t>(0), this->m_capacity)) (*this)[i] = data[i];
		}

		MappableBuffer(const MappableBuffer& source, ID3D12GraphicsCommandList* commandList) noexcept(false) : GPUBuffer<T, MappableBuffer::HeapType, Alignment>(source, commandList) { Map(); }

		MappableBuffer(MappableBuffer&& source) noexcept { Swap(source); }
		MappableBuffer&& operator=(MappableBuffer&& source) noexcept {
			Swap(source);
			return move(*this);
		}

		void Map() { if (m_data == nullptr) ThrowIfFailed(this->m_resource->Map(0, nullptr, reinterpret_cast<void**>(&m_data))); }

		void Unmap() {
			if (m_data != nullptr) {
				this->m_resource->Unmap(0, nullptr);
				m_data = nullptr;
			}
		}

		const T& operator[](size_t index) const {
			if (index >= this->m_capacity) throw out_of_range("");
			return *reinterpret_cast<const T*>(m_data + this->ItemSize * index);
		}
		T& operator[](size_t index) { return const_cast<T&>(as_const(*this)[index]); }

		const T& GetData(size_t index = 0) const { return (*this)[index]; }
		T& GetData(size_t index = 0) { return (*this)[index]; }

	protected:
		PBYTE m_data{};

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
	struct ConstantBuffer : UploadBuffer<T, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT> {
		using UploadBuffer<T, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT>::UploadBuffer;

		void CreateConstantBufferView(D3D12_CPU_DESCRIPTOR_HANDLE descriptor) const {
			ComPtr<ID3D12Device> device;
			ThrowIfFailed(this->m_resource->GetDevice(IID_PPV_ARGS(&device)));
			const D3D12_CONSTANT_BUFFER_VIEW_DESC constantBufferViewDesc{
				.BufferLocation = this->m_resource->GetGPUVirtualAddress(),
				.SizeInBytes = static_cast<UINT>(this->m_resource->GetDesc().Width)
			};
			device->CreateConstantBufferView(&constantBufferViewDesc, descriptor);
		}
	};

	template <typename T>
	struct StructuredBuffer : UploadBuffer<T> {
		using UploadBuffer<T>::UploadBuffer;

		void CreateShaderResourceView(D3D12_CPU_DESCRIPTOR_HANDLE descriptor) const {
			ComPtr<ID3D12Device> device;
			ThrowIfFailed(this->m_resource->GetDevice(IID_PPV_ARGS(&device)));
			CreateBufferShaderResourceView(device.Get(), this->m_resource.Get(), descriptor, this->ItemSize);
		}
	};

	template <typename T>
	struct RWStructuredBuffer : GPUBuffer<T> {
		using GPUBuffer<T>::GPUBuffer;

		RWStructuredBuffer(
			ID3D12Device* pDevice,
			size_t capacity = 1,
			D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAGS additionalFlags = D3D12_RESOURCE_FLAG_NONE
		) noexcept(false) : GPUBuffer<T>(pDevice, capacity, initialState, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | additionalFlags) {}

		RWStructuredBuffer(
			ID3D12Device* pDevice, ResourceUploadBatch& resourceUploadBatch,
			span<const T> data,
			D3D12_RESOURCE_STATES afterState = D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAGS additionalFlags = D3D12_RESOURCE_FLAG_NONE
		) noexcept(false) {
			this->m_capacity = size(data);
			this->m_state = afterState;
			const auto flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | additionalFlags;
			if (sizeof(T) % 2 == 0) ThrowIfFailed(CreateStaticBuffer(pDevice, resourceUploadBatch, data, afterState, &this->m_resource, flags));
			else {
				vector<BYTE> newData(this->ItemSize * this->m_capacity);
				for (const auto i : views::iota(static_cast<size_t>(0), this->m_capacity)) *reinterpret_cast<T*>(::data(newData) + this->ItemSize * i) = data[i];
				ThrowIfFailed(CreateStaticBuffer(pDevice, resourceUploadBatch, newData, afterState, &this->m_resource, flags));
			}
		}

		void CreateShaderResourceView(D3D12_CPU_DESCRIPTOR_HANDLE descriptor) const {
			ComPtr<ID3D12Device> device;
			ThrowIfFailed(this->m_resource->GetDevice(IID_PPV_ARGS(&device)));
			CreateBufferShaderResourceView(device.Get(), this->m_resource.Get(), descriptor, this->ItemSize);
		}

		void CreateUnorderedAccessView(D3D12_CPU_DESCRIPTOR_HANDLE descriptor) const {
			ComPtr<ID3D12Device> device;
			ThrowIfFailed(this->m_resource->GetDevice(IID_PPV_ARGS(&device)));
			CreateBufferUnorderedAccessView(device.Get(), this->m_resource.Get(), descriptor, this->ItemSize);
		}
	};
}
