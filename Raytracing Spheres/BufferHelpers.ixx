module;

#include "pch.h"

#include "directxtk12/DirectXHelpers.h"

export module DirectX.BufferHelpers;

using namespace DirectX;
using namespace DX;
using namespace Microsoft::WRL;

export namespace DirectX::BufferHelpers {
	template <typename T>
	class UploadBuffer {
	public:
		using ItemType = T;

		const size_t ItemSize;

		const bool IsConstantBuffer;

		UploadBuffer(ID3D12Device* pDevice, bool isConstantBuffer, size_t count = 1, D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_GENERIC_READ) noexcept(false) :
			IsConstantBuffer(isConstantBuffer),
			ItemSize(isConstantBuffer ? AlignUp(sizeof(T), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT) : sizeof(T)) {
			const CD3DX12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE_UPLOAD);
			const auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(ItemSize * count);
			ThrowIfFailed(pDevice->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &resourceDesc, initialState, nullptr, IID_PPV_ARGS(&m_buffer)));
			ThrowIfFailed(m_buffer->Map(0, nullptr, reinterpret_cast<void**>(&m_data)));
		}

		~UploadBuffer() { m_buffer->Unmap(0, nullptr); }

		ID3D12Resource* GetResource() const { return m_buffer.Get(); }

		T& operator[](size_t index) { return *reinterpret_cast<T*>(m_data + ItemSize * index); }
		const T& operator[](size_t index) const { return *reinterpret_cast<const T*>(m_data + ItemSize * index); }

		T& GetData(size_t index = 0) { return (*this)[index]; }
		const T& GetData(size_t index = 0) const { return (*this)[index]; }

	private:
		ComPtr<ID3D12Resource> m_buffer;
		PBYTE m_data{};
	};
}
