module;

#include "pch.h"

export module DirectX.BufferHelpers;

using namespace DX;
using namespace Microsoft::WRL;

export namespace DirectX::BufferHelpers {
	void CreateConstantBufferView(ID3D12Resource* pResource, D3D12_CPU_DESCRIPTOR_HANDLE descriptor, UINT stride, UINT count = 1) {
		ComPtr<ID3D12Device> device;
		ThrowIfFailed(pResource->GetDevice(IID_PPV_ARGS(&device)));
		const D3D12_CONSTANT_BUFFER_VIEW_DESC constantBufferViewDesc{
			.BufferLocation = pResource->GetGPUVirtualAddress(),
			.SizeInBytes = stride * count
		};
		device->CreateConstantBufferView(&constantBufferViewDesc, descriptor);
	}

	void CreateShaderResourceView(ID3D12Resource* pResource, D3D12_CPU_DESCRIPTOR_HANDLE descriptor, UINT stride, UINT count = 1) {
		ComPtr<ID3D12Device> device;
		ThrowIfFailed(pResource->GetDevice(IID_PPV_ARGS(&device)));
		const D3D12_SHADER_RESOURCE_VIEW_DESC shaderResourceViewDesc{
			.ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
			.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
			.Buffer{
				.NumElements = count,
				.StructureByteStride = stride
			}
		};
		device->CreateShaderResourceView(pResource, &shaderResourceViewDesc, descriptor);
	}

	void CreateUnorderedAccessView(ID3D12Resource* pResource, D3D12_CPU_DESCRIPTOR_HANDLE descriptor, UINT stride, UINT count = 1) {
		ComPtr<ID3D12Device> device;
		ThrowIfFailed(pResource->GetDevice(IID_PPV_ARGS(&device)));
		const D3D12_UNORDERED_ACCESS_VIEW_DESC unorderedAccessDesc{
			.ViewDimension = D3D12_UAV_DIMENSION_BUFFER,
			.Buffer{
				.NumElements = count,
				.StructureByteStride = stride
			}
		};
		device->CreateUnorderedAccessView(pResource, nullptr, &unorderedAccessDesc, descriptor);
	}

	template <typename T, D3D12_HEAP_TYPE Type = D3D12_HEAP_TYPE_DEFAULT, UINT Alignment = 2>
	class GPUBuffer {
	public:
		static_assert(Alignment % 2 == 0);

		using ItemType = T;

		static constexpr D3D12_HEAP_TYPE HeapType = Type;

		static constexpr UINT ItemSize = (sizeof(T) + Alignment - 1) & ~(Alignment - 1);

		const UINT Count;

		GPUBuffer(GPUBuffer&) = delete;
		GPUBuffer& operator=(GPUBuffer&) = delete;

		GPUBuffer(ID3D12Device* pDevice, UINT count = 1, D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) noexcept(false) : Count(count) {
			if (count) {
				const CD3DX12_HEAP_PROPERTIES heapProperties(Type);
				const auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(ItemSize * count, flags);
				ThrowIfFailed(pDevice->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &resourceDesc, initialState, nullptr, IID_PPV_ARGS(&m_buffer)));
			}
		}

		ID3D12Resource* GetResource() const noexcept { return m_buffer.Get(); }

	protected:
		ComPtr<ID3D12Resource> m_buffer;
	};

	template <typename T, bool IsUpload = true, UINT Alignment = 2>
	class MappableBuffer : public GPUBuffer<T, IsUpload ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_READBACK, Alignment> {
	public:
		MappableBuffer(ID3D12Device* pDevice, UINT count = 1) noexcept(false) :
			GPUBuffer<T, MappableBuffer::HeapType, Alignment>(pDevice, count, IsUpload ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST) {
			ThrowIfFailed(this->m_buffer->Map(0, nullptr, reinterpret_cast<void**>(&m_data)));
		}

		const T& operator[](UINT index) const noexcept { return *reinterpret_cast<const T*>(m_data + this->ItemSize * index); }
		T& operator[](UINT index) noexcept { return *reinterpret_cast<T*>(m_data + this->ItemSize * index); }

		const T& GetData(UINT index = 0) const noexcept { return (*this)[index]; }
		T& GetData(UINT index = 0) noexcept { return (*this)[index]; }

	private:
		PBYTE m_data;
	};

	template <typename T, UINT Alignment = 2>
	using UploadBuffer = MappableBuffer<T, true, Alignment>;

	template <typename T, UINT Alignment = 2>
	using ReadbackBuffer = MappableBuffer<T, false, Alignment>;

	template <typename T>
	struct ConstantBuffer : UploadBuffer<T, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT> {
		ConstantBuffer(ID3D12Device* pDevice, UINT count = 1) noexcept(false) : UploadBuffer<T, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT>(pDevice, count) {}

		void CreateConstantBufferView(D3D12_CPU_DESCRIPTOR_HANDLE descriptor) const { ::CreateConstantBufferView(this->m_buffer.Get(), descriptor, this->ItemSize, this->Count); }
	};

	template <typename T>
	struct StructuredBuffer : UploadBuffer<T> {
		StructuredBuffer(ID3D12Device* pDevice, UINT count = 1) noexcept(false) : UploadBuffer<T>(pDevice, count) {}

		void CreateShaderResourceView(D3D12_CPU_DESCRIPTOR_HANDLE descriptor) const { ::CreateShaderResourceView(this->m_buffer.Get(), descriptor, this->ItemSize, this->Count); }
	};

	template <typename T>
	struct RWStructuredBuffer : GPUBuffer<T> {
		RWStructuredBuffer(ID3D12Device* pDevice, UINT count = 1) noexcept(false) : GPUBuffer<T>(pDevice, count, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) {}

		void CreateShaderResourceView(D3D12_CPU_DESCRIPTOR_HANDLE descriptor) const { ::CreateShaderResourceView(this->m_buffer.Get(), descriptor, this->ItemSize, this->Count); }

		void CreateUnorderedAccessView(D3D12_CPU_DESCRIPTOR_HANDLE descriptor) const { ::CreateUnorderedAccessView(this->m_buffer.Get(), descriptor, this->ItemSize, this->Count); }
	};
}
