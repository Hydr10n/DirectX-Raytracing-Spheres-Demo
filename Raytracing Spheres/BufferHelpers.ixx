module;

#include "pch.h"

export module DirectX.BufferHelpers;

using namespace DX;
using namespace Microsoft::WRL;

export namespace DirectX::BufferHelpers {
	template <typename T, size_t Alignment = 16>
	class UploadBuffer {
	public:
		static_assert(Alignment % 16 == 0);

		using ItemType = T;

		static constexpr size_t ItemSize = (sizeof(T) + Alignment - 1) & ~(Alignment - 1);

		const size_t Count;

		UploadBuffer(UploadBuffer&) = delete;
		UploadBuffer& operator=(UploadBuffer&) = delete;

		UploadBuffer(ID3D12Device* pDevice, size_t count = 1, D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_GENERIC_READ) noexcept(false) : Count(count), m_device(pDevice) {
			const CD3DX12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE_UPLOAD);
			const auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(ItemSize * count);
			ThrowIfFailed(pDevice->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &resourceDesc, initialState | D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_buffer)));
			ThrowIfFailed(m_buffer->Map(0, nullptr, reinterpret_cast<void**>(&m_data)));
		}

		~UploadBuffer() { m_buffer->Unmap(0, nullptr); }

		ID3D12Resource* GetResource() const { return m_buffer.Get(); }

		T& operator[](size_t index) { return *reinterpret_cast<T*>(m_data + ItemSize * index); }
		const T& operator[](size_t index) const { return *reinterpret_cast<const T*>(m_data + ItemSize * index); }

		T& GetData(size_t index = 0) { return (*this)[index]; }
		const T& GetData(size_t index = 0) const { return (*this)[index]; }

	protected:
		ID3D12Device* m_device;

		ComPtr<ID3D12Resource> m_buffer;
		PBYTE m_data;
	};

	template <typename T>
	struct ConstantBuffer : UploadBuffer<T, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT> {
		ConstantBuffer(ID3D12Device* pDevice, size_t count = 1, D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_GENERIC_READ) noexcept(false) :
			UploadBuffer<T, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT>(pDevice, count, initialState) {}

		void CreateConstantBufferView(D3D12_CPU_DESCRIPTOR_HANDLE descriptor) const {
			const D3D12_CONSTANT_BUFFER_VIEW_DESC constantBufferViewDesc{
				.BufferLocation = this->m_buffer->GetGPUVirtualAddress(),
				.SizeInBytes = static_cast<UINT>(this->ItemSize * this->Count)
			};
			this->m_device->CreateConstantBufferView(&constantBufferViewDesc, descriptor);
		}
	};

	template <typename T>
	struct StructuredBuffer : UploadBuffer<T> {
		StructuredBuffer(ID3D12Device* pDevice, size_t count = 1, D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_GENERIC_READ) noexcept(false) :
			UploadBuffer<T>(pDevice, count, initialState) {}

		void CreateShaderResourceView(D3D12_CPU_DESCRIPTOR_HANDLE descriptor) const {
			const D3D12_SHADER_RESOURCE_VIEW_DESC shaderResourceViewDesc{
				.ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
				.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
				.Buffer{
					.NumElements = static_cast<UINT>(this->Count),
					.StructureByteStride = static_cast<UINT>(this->ItemSize)
				}
			};
			this->m_device->CreateShaderResourceView(this->m_buffer.Get(), &shaderResourceViewDesc, descriptor);
		}
	};

	template <typename T>
	struct RWStructuredBuffer : StructuredBuffer<T> {
		RWStructuredBuffer(ID3D12Device* pDevice, size_t count = 1, D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_GENERIC_READ) noexcept(false) :
			StructuredBuffer<T>(pDevice, count, initialState) {}

		void CreateUnorderedAccessView(D3D12_CPU_DESCRIPTOR_HANDLE descriptor) {
			const D3D12_UNORDERED_ACCESS_VIEW_DESC unorderedAccessDesc{
				.ViewDimension = D3D12_UAV_DIMENSION_BUFFER,
				.Buffer{
					.NumElements = static_cast<UINT>(this->Count),
					.StructureByteStride = static_cast<UINT>(this->ItemSize)
				}
			};
			this->m_device->CreateUnorderedAccessView(this->m_buffer.Get(), nullptr, &unorderedAccessDesc, descriptor);
		}
	};
}
