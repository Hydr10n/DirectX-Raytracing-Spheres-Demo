module;

#include <ranges>
#include <stdexcept>

#include "directx/d3dx12.h"

#include "D3D12MemAlloc.h"

#include "FormatHelpers.h"

export module GPUBuffer;

import DescriptorHeap;
import DeviceContext;
import ErrorHelpers;
import GPUResource;

using namespace D3D12MA;
using namespace DirectX::FormatHelpers;
using namespace ErrorHelpers;
using namespace Microsoft::WRL;
using namespace std;

export namespace DirectX {
	struct BufferRange {
		UINT64 Offset{}, Size = ~0ull;

		constexpr bool IsEntire(UINT64 size) const { return !Offset && (Size == ~0ull || size == Size); }

		constexpr BufferRange Resolve(UINT64 size) const {
			BufferRange ret;
			ret.Offset = min(Offset, size);
			ret.Size = Size ? min(Size, size - ret.Offset) : size - ret.Offset;
			return ret;
		}
	};

	enum class GPUBufferType { Default, Upload, Readback, RaytracingAccelerationStructure };

	enum class BufferSRVType { Raw, Structured, Typed };
	enum class BufferUAVType { Raw, Structured, Typed, Clear };

	class GPUBuffer : public GPUResource {
	public:
		struct CreationDesc {
			GPUBufferType Type = GPUBufferType::Default;
			DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
			UINT64 Size{};
			UINT Stride{};
			D3D12_RESOURCE_FLAGS Flags = D3D12_RESOURCE_FLAG_NONE;
			D3D12_RESOURCE_STATES InitialState = D3D12_RESOURCE_STATE_COMMON;
			bool KeepInitialState{};
		};

		GPUBuffer(
			const DeviceContext& deviceContext,
			ID3D12Resource* pResource,
			D3D12_RESOURCE_STATES initialState, bool keepInitialState,
			UINT stride,
			DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN
		) noexcept(false) :
			GPUResource(deviceContext, pResource, initialState, keepInitialState),
			m_type(
				initialState == D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE ?
				GPUBufferType::RaytracingAccelerationStructure : GetBufferType(GetHeapType(pResource))
			),
			m_format(format),
			m_stride(stride) {
			if ((*this)->GetDesc().Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) Throw<invalid_argument>("Resource is not buffer");

			CheckStride(pResource->GetDesc().Width);
		}

		GPUBuffer(const DeviceContext& deviceContext, const CreationDesc& creationDesc) noexcept(false) :
			GPUResource(deviceContext, creationDesc.InitialState, creationDesc.KeepInitialState),
			m_type(creationDesc.Type),
			m_format(creationDesc.Format),
			m_stride(creationDesc.Stride) {
			CheckStride(creationDesc.Size);

			const ALLOCATION_DESC allocationDesc{ .HeapType = GetHeapType(creationDesc.Type) };
			const auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(creationDesc.Size, creationDesc.Flags);
			ThrowIfFailed(deviceContext.MemoryAllocator->CreateResource(&allocationDesc, &resourceDesc, creationDesc.InitialState, nullptr, &m_allocation, IID_NULL, nullptr));
		}

		template<typename T>
		static unique_ptr<GPUBuffer> CreateDefault(
			const DeviceContext& deviceContext,
			UINT64 capacity,
			DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN,
			bool keepInitialState = false
		) {
			return make_unique<GPUBuffer>(
				deviceContext,
				CreationDesc{
					.Format = format,
					.Size = sizeof(T) * capacity,
					.Stride = sizeof(T),
					.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
					.KeepInitialState = keepInitialState
				}
			);
		}

		template <typename T>
		static unique_ptr<GPUBuffer> CreateUpload(
			const DeviceContext& deviceContext,
			UINT64 capacity,
			DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN,
			bool keepInitialState = false
		) {
			return make_unique<GPUBuffer>(
				deviceContext,
				CreationDesc{
					.Type = GPUBufferType::Upload,
					.Format = format,
					.Size = sizeof(T) * capacity,
					.Stride = sizeof(T),
					.InitialState = D3D12_RESOURCE_STATE_GENERIC_READ,
					.KeepInitialState = keepInitialState
				}
			);
		}

		template <typename T, bool IsDefault = true> requires (sizeof(T) % D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT == 0)
			static unique_ptr<GPUBuffer> CreateConstant(
				const DeviceContext& deviceContext, UINT64 capacity = 1,
				bool keepInitialState = false
			) {
			return make_unique<GPUBuffer>(
				deviceContext,
				CreationDesc{
					.Type = IsDefault ? GPUBufferType::Default : GPUBufferType::Upload,
					.Size = sizeof(T) * capacity,
					.Stride = sizeof(T),
					.InitialState = IsDefault ? D3D12_RESOURCE_STATE_COMMON : D3D12_RESOURCE_STATE_GENERIC_READ,
					.KeepInitialState = keepInitialState
				}
			);
		}

		static unique_ptr<GPUBuffer> CreateReadback(
			const DeviceContext& deviceContext,
			UINT64 size,
			bool keepInitialState = false
		) {
			return make_unique<GPUBuffer>(
				deviceContext,
				CreationDesc{
					.Type = GPUBufferType::Readback,
					.Size = size,
					.InitialState = D3D12_RESOURCE_STATE_COPY_DEST,
					.KeepInitialState = keepInitialState
				}
			);
		}

		static unique_ptr<GPUBuffer> CreateRaytracingAccelerationStructure(
			const DeviceContext& deviceContext,
			UINT64 size,
			bool keepInitialState = false
		) {
			return make_unique<GPUBuffer>(
				deviceContext,
				CreationDesc{
					.Type = GPUBufferType::RaytracingAccelerationStructure,
					.Size = size,
					.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
					.InitialState = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
					.KeepInitialState = keepInitialState
				}
			);
		}

		GPUBufferType GetType() const noexcept { return m_type; }

		UINT GetStride() const noexcept { return m_stride; }

		size_t GetCapacity() const noexcept { return (*this)->GetDesc().Width / m_stride; }

		bool IsMappable() const noexcept {
			return m_type == GPUBufferType::Upload || m_type == GPUBufferType::Readback || m_deviceContext.MemoryAllocator->IsGPUUploadHeapSupported();
		}

		void* GetMappedData() {
			Map();
			return m_mappedData;
		}

		void Map() {
			if (m_mappedData == nullptr) ThrowIfFailed((*this)->Map(0, nullptr, &m_mappedData));
		}

		void Unmap() {
			if (m_mappedData != nullptr) {
				(*this)->Unmap(0, nullptr);
				m_mappedData = nullptr;
			}
		}

		const Descriptor& GetCBVDescriptor() const noexcept { return *m_descriptors.CBV; }
		const Descriptor& GetSRVDescriptor(BufferSRVType type) const noexcept { return *m_descriptors.SRV[to_underlying(type)]; }
		const Descriptor& GetUAVDescriptor(BufferUAVType type) const noexcept { return *m_descriptors.UAV[to_underlying(type)]; }

		void CreateCBV(BufferRange range = {}) {
			const auto resource = GetNative();

			range = range.Resolve(resource->GetDesc().Width);

			const D3D12_CONSTANT_BUFFER_VIEW_DESC desc{
				.BufferLocation = resource->GetGPUVirtualAddress() + range.Offset,
				.SizeInBytes = static_cast<UINT>(range.Size)
			};

			auto& descriptor = m_descriptors.CBV;
			descriptor = m_deviceContext.ResourceDescriptorHeap->Allocate();
			m_deviceContext.Device->CreateConstantBufferView(&desc, *descriptor);
		}

		void CreateSRV(BufferSRVType type, BufferRange range = {}) {
			const auto resource = GetNative();

			D3D12_SHADER_RESOURCE_VIEW_DESC desc{
				.ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
				.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING
			};
			UINT stride;
			switch (type) {
				case BufferSRVType::Raw:
				{
					desc.Format = DXGI_FORMAT_R32_TYPELESS;
					desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
					stride = sizeof(UINT32);
				}
				break;

				case BufferSRVType::Structured:
				{
					desc.Buffer.StructureByteStride = m_stride;
					stride = m_stride;
				}
				break;

				case BufferSRVType::Typed:
				{
					desc.Format = m_format;
					stride = GetBits(m_format) / 8;
				}
				break;
			}
			range = range.Resolve(resource->GetDesc().Width);
			desc.Buffer.FirstElement = range.Offset / stride;
			desc.Buffer.NumElements = static_cast<UINT>(range.Size) / stride;

			auto& descriptor = m_descriptors.SRV[to_underlying(type)];
			descriptor = m_deviceContext.ResourceDescriptorHeap->Allocate();
			m_deviceContext.Device->CreateShaderResourceView(resource, &desc, *descriptor);
		}

		void CreateUAV(BufferUAVType type, BufferRange range = {}) {
			const auto resource = GetNative();

			D3D12_UNORDERED_ACCESS_VIEW_DESC desc{ .ViewDimension = D3D12_UAV_DIMENSION_BUFFER };
			UINT stride;
			switch (type) {
				case BufferUAVType::Clear:
				case BufferUAVType::Raw:
				{
					desc.Format = DXGI_FORMAT_R32_TYPELESS;
					desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
					stride = sizeof(UINT32);
				}
				break;

				case BufferUAVType::Structured:
				{
					desc.Buffer.StructureByteStride = m_stride;
					stride = m_stride;
				}
				break;

				case BufferUAVType::Typed:
				{
					desc.Format = m_format;
					stride = GetBits(m_format) / 8;
				}
				break;
			}
			range = range.Resolve(resource->GetDesc().Width);
			desc.Buffer.FirstElement = range.Offset / stride;
			desc.Buffer.NumElements = static_cast<UINT>(range.Size) / stride;

			auto& descriptor = m_descriptors.UAV[to_underlying(type)];
			descriptor = (type == BufferUAVType::Clear ? m_deviceContext.DefaultDescriptorHeap : m_deviceContext.ResourceDescriptorHeap)->Allocate();
			m_deviceContext.Device->CreateUnorderedAccessView(resource, nullptr, &desc, *descriptor);

			if (type == BufferUAVType::Clear && !m_descriptors.UAV[to_underlying(BufferUAVType::Raw)]) {
				CreateUAV(BufferUAVType::Raw);
			}
		}

	private:
		GPUBufferType m_type;

		DXGI_FORMAT m_format;

		UINT m_stride;

		void* m_mappedData{};

		struct { unique_ptr<Descriptor> CBV, SRV[3], UAV[4]; } m_descriptors;

		static D3D12_HEAP_TYPE GetHeapType(ID3D12Resource* pResource) {
			D3D12_HEAP_PROPERTIES heapProperties;
			ThrowIfFailed(pResource->GetHeapProperties(&heapProperties, nullptr));
			return heapProperties.Type;
		}

		D3D12_HEAP_TYPE GetHeapType(GPUBufferType type) const {
			const auto isGPUUploadHeapSupported = m_deviceContext.MemoryAllocator->IsGPUUploadHeapSupported();
			switch (type) {
				case GPUBufferType::Upload: return isGPUUploadHeapSupported ? D3D12_HEAP_TYPE_GPU_UPLOAD : D3D12_HEAP_TYPE_UPLOAD;
				case GPUBufferType::Readback: return D3D12_HEAP_TYPE_READBACK;
				default: return isGPUUploadHeapSupported ? D3D12_HEAP_TYPE_GPU_UPLOAD : D3D12_HEAP_TYPE_DEFAULT;
			}
		}

		static constexpr GPUBufferType GetBufferType(D3D12_HEAP_TYPE type) {
			switch (type) {
				case D3D12_HEAP_TYPE_UPLOAD: return GPUBufferType::Upload;
				case D3D12_HEAP_TYPE_READBACK: return GPUBufferType::Readback;
				default: return GPUBufferType::Default;
			}
		}

		void CheckStride(UINT64 size) const {
			if (m_type == GPUBufferType::Default || m_type == GPUBufferType::Upload) {
				if (!m_stride) Throw<invalid_argument>("Buffer stride cannot be 0");
				if (size % m_stride != 0) Throw<invalid_argument>("Buffer size unaligned to stride");
			}
		}
	};
}
