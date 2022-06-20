#pragma once

#include "DirectXTK12/GraphicsMemory.h"

#include "DirectXTK12/DirectXHelpers.h"

namespace DirectX {
	template <class T>
	struct GraphicsResourceEx : GraphicsResource {
		using GraphicsResource::GraphicsResource;

		GraphicsResourceEx(GraphicsResource&& graphicsResource) : GraphicsResource(std::move(graphicsResource)) {}

		T* Memory() const noexcept { return reinterpret_cast<T*>(GraphicsResource::Memory()); }
	};

	template <class T>
	struct GraphicsResourceArray : GraphicsResource {
		using GraphicsResource::GraphicsResource;

		GraphicsResourceArray(GraphicsResource&& graphicsResource, size_t alignment = 16) :
			GraphicsResource(std::move(graphicsResource)), m_alignedSize(AlignUp(sizeof(T), alignment)) {}

		T* Memory(size_t i = 0) const noexcept {
			return reinterpret_cast<T*>(reinterpret_cast<PBYTE>(GraphicsResource::Memory()) + m_alignedSize * i);
		}

		D3D12_GPU_VIRTUAL_ADDRESS GpuAddress(size_t i = 0) const noexcept { return GraphicsResource::GpuAddress() + m_alignedSize * i; }

		size_t Length() const noexcept { return *this ? Size() / m_alignedSize : 0; }

	private:
		size_t m_alignedSize = AlignUp(sizeof(T), 16);
	};

	template <class T>
	struct SharedGraphicsResourceEx : SharedGraphicsResource {
		using SharedGraphicsResource::SharedGraphicsResource;

		SharedGraphicsResourceEx(SharedGraphicsResource&& graphicsResource) : SharedGraphicsResource(std::move(graphicsResource)) {}

		T* Memory() const noexcept { return static_cast<T*>(SharedGraphicsResource::Memory()); }
	};

	template <class T>
	struct SharedGraphicsResourceArray : SharedGraphicsResource {
		using SharedGraphicsResource::SharedGraphicsResource;

		SharedGraphicsResourceArray(SharedGraphicsResource&& graphicsResource, size_t alignment = 16) :
			SharedGraphicsResource(std::move(graphicsResource)), m_alignedSize(AlignUp(sizeof(T), alignment)) {}

		T* Memory(size_t i = 0) const noexcept {
			return reinterpret_cast<T*>(static_cast<PBYTE>(SharedGraphicsResource::Memory()) + m_alignedSize * i);
		}

		D3D12_GPU_VIRTUAL_ADDRESS GpuAddress(size_t i = 0) const noexcept { return SharedGraphicsResource::GpuAddress() + m_alignedSize * i; }

		size_t Length() const noexcept { return *this ? Size() / m_alignedSize : 0; }

	private:
		size_t m_alignedSize = AlignUp(sizeof(T), 16);
	};

	struct GraphicsMemoryEx : GraphicsMemory {
		using GraphicsMemory::GraphicsMemory;

		template<class T>
		GraphicsResourceEx<T> AllocateConstant() { return std::move(GraphicsMemory::AllocateConstant<T>()); }

		template<class T>
		GraphicsResourceEx<T> AllocateConstant(const T& data) { return std::move(GraphicsMemory::AllocateConstant(data)); }

		template<class T>
		GraphicsResourceArray<T> AllocateConstants(size_t count) {
			constexpr auto Alignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
			return GraphicsResourceArray<T>(Allocate(AlignUp(sizeof(T), Alignment) * count), Alignment);
		}

		template<class T>
		GraphicsResourceArray<T> AllocateConstants(const T* data, size_t count) {
			auto constants = AllocateConstants<T>(count);
			if (sizeof(T) == AlignUp(sizeof(T), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT)) {
				memcpy(constants.Memory(), data, sizeof(T) * count);
			}
			else { for (size_t i = 0; i < count; i++) { memcpy(constants.Memory(i), data + i, sizeof(T)); } }
			return constants;
		}
	};
}
