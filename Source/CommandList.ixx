module;

#include <span>
#include <stdexcept>
#include <unordered_set>

#include <wrl.h>

#include "directx/d3dx12.h"

#include "directxtk12/DirectXHelpers.h"

#include "D3D12MemAlloc.h"

#include "rtxmu/D3D12AccelStructManager.h"

export module CommandList;

import DeviceContext;
import ErrorHelpers;
import GPUBuffer;
import GPUResource;
import Texture;

using namespace D3D12MA;
using namespace ErrorHelpers;
using namespace Microsoft::WRL;
using namespace Microsoft::WRL::Wrappers;
using namespace std;

#define COPY() \
	if (sizeof(T) != buffer.GetStride()) Throw<runtime_error>("Buffer tride mismatch"); \
	Copy(buffer, ::data(data), sizeof(T) * size(data), offset);

export namespace DirectX {
	class CommandList {
	public:
		CommandList(const CommandList&) = delete;
		CommandList& operator=(const CommandList&) = delete;

		CommandList(const DeviceContext& deviceContext, D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT) noexcept(false) :
			m_deviceContext(deviceContext) {
			ThrowIfFailed(deviceContext.Device->CreateCommandAllocator(type, IID_PPV_ARGS(&m_commandAllocator)));
			ThrowIfFailed(deviceContext.Device->CreateCommandList(0, type, m_commandAllocator.Get(), nullptr, IID_PPV_ARGS(&m_commandList)));
			ThrowIfFailed(m_commandList->Close());

			ThrowIfFailed(deviceContext.Device->CreateFence(m_fenceValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
			m_fenceEvent.Attach(CreateEvent(nullptr, FALSE, FALSE, nullptr));
			ThrowIfFailed(static_cast<BOOL>(m_fenceEvent.IsValid()));

			constexpr POOL_DESC poolDesc{
				.Flags = POOL_FLAG_ALGORITHM_LINEAR,
				.HeapProperties{
					.Type = D3D12_HEAP_TYPE_UPLOAD
				},
				.HeapFlags = D3D12_HEAP_FLAG_CREATE_NOT_ZEROED
			};
			ThrowIfFailed(deviceContext.MemoryAllocator->CreatePool(&poolDesc, &m_pool));
		}

		~CommandList() {
			Wait();

			for (auto& ID : m_compactAccelerationStructureIDs) {
				if (!empty(ID)) m_deviceContext.AccelerationStructureManager->GarbageCollection(ID);
			}
		}

		auto GetNative() const noexcept { return m_commandList.Get(); }
		auto operator->() const noexcept { return m_commandList.Get(); }
		operator ID3D12GraphicsCommandList4* () const noexcept { return m_commandList.Get(); }

		const DeviceContext& GetDeviceContext() const noexcept { return m_deviceContext; }

		void Begin() {
			ThrowIfFailed(m_commandAllocator->Reset());
			ThrowIfFailed(m_commandList->Reset(m_commandAllocator.Get(), nullptr));

			SetResourceDescriptorHeap();
		}

		void End(bool wait = true) {
			for (auto& resource : m_trackedResources) SetState(*resource, resource->GetInitialState());
			m_trackedResources.clear();

			ThrowIfFailed(m_commandList->Close());
			m_deviceContext.CommandQueue->ExecuteCommandLists(1, CommandListCast(m_commandList.GetAddressOf()));

			if (!empty(m_compactAccelerationStructureIDs[1])) {
				m_compactAccelerationStructureIDs[0].append_range(m_compactAccelerationStructureIDs[1]);
				m_compactAccelerationStructureIDs[1].clear();
			}

			if (wait) Wait();
		}

		void Wait() {
			const auto fenceValue = ++m_fenceValue;
			ThrowIfFailed(m_deviceContext.CommandQueue->Signal(m_fence.Get(), fenceValue));
			if (m_fence->GetCompletedValue() < fenceValue) {
				ThrowIfFailed(m_fence->SetEventOnCompletion(fenceValue, m_fenceEvent.Get()));
				ignore = WaitForSingleObject(m_fenceEvent.Get(), INFINITE);
			}

			m_trackedAllocations.clear();

			if (!empty(m_builtAccelerationStructureIDs)) {
				m_deviceContext.AccelerationStructureManager->GarbageCollection(m_builtAccelerationStructureIDs);
				m_builtAccelerationStructureIDs.clear();
			}
		}

		void SetResourceDescriptorHeap() {
			const auto descriptorHeap = m_deviceContext.ResourceDescriptorHeap->Heap();
			m_commandList->SetDescriptorHeaps(1, &descriptorHeap);
		}

		void SetUAVBarrier(GPUResource& resource) {
			const auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(resource);
			(*this)->ResourceBarrier(1, &barrier);
		}

		void SetState(GPUResource& resource, D3D12_RESOURCE_STATES state) {
			if (resource.GetState() != state) {
				TransitionResource(*this, resource, resource.GetState(), state);
				resource.SetState(state);

				if (resource.KeepInitialState()) m_trackedResources.emplace(&resource);
			}
			else if (state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) SetUAVBarrier(resource);
		}

		void Clear(GPUBuffer& buffer, UINT value = 0) {
			SetState(buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			(*this)->ClearUnorderedAccessViewUint(buffer.GetUAVDescriptor(BufferUAVType::Raw), buffer.GetUAVDescriptor(BufferUAVType::Clear), buffer, data(initializer_list{ value, value, value, value }), 0, nullptr);
			SetUAVBarrier(buffer);
		}

		void Copy(GPUBuffer& destination, GPUBuffer& source) {
			SetState(destination, D3D12_RESOURCE_STATE_COPY_DEST);
			SetState(source, D3D12_RESOURCE_STATE_COPY_SOURCE);
			(*this)->CopyResource(destination, source);
		}

		void Copy(GPUBuffer& destination, UINT64 destinationOffset, GPUBuffer& source, UINT64 sourceOffset, UINT64 size) {
			SetState(destination, D3D12_RESOURCE_STATE_COPY_DEST);
			SetState(source, D3D12_RESOURCE_STATE_COPY_SOURCE);
			(*this)->CopyBufferRegion(destination, destinationOffset, source, sourceOffset, size);
		}

		void Copy(GPUBuffer& buffer, const void* pData, size_t size, size_t offset = 0) {
			const auto bufferRange = BufferRange{ offset, size }.Resolve(buffer->GetDesc().Width);

			if (buffer.IsMappable()) {
				memcpy(buffer.GetMappedData(), pData, bufferRange.Size);

				return;
			}

			if (!m_pool) {
				constexpr POOL_DESC poolDesc{
					.Flags = POOL_FLAG_ALGORITHM_LINEAR,
					.HeapProperties{
						.Type = D3D12_HEAP_TYPE_UPLOAD
					},
					.HeapFlags = D3D12_HEAP_FLAG_CREATE_NOT_ZEROED
				};
				ThrowIfFailed(m_deviceContext.MemoryAllocator->CreatePool(&poolDesc, &m_pool));
			}

			ComPtr<Allocation> allocation;
			const ALLOCATION_DESC allocationDesc{ .Flags = ALLOCATION_FLAG_STRATEGY_MIN_TIME, .CustomPool = m_pool.Get() };
			const auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(bufferRange.Size);
			ThrowIfFailed(m_deviceContext.MemoryAllocator->CreateResource(&allocationDesc, &resourceDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, &allocation, IID_NULL, nullptr));
			const auto resource = allocation->GetResource();

			constexpr D3D12_RANGE range{};
			void* data;
			ThrowIfFailed(resource->Map(0, &range, &data));
			memcpy(data, pData, bufferRange.Size);

			SetState(buffer, D3D12_RESOURCE_STATE_COPY_DEST);
			(*this)->CopyBufferRegion(buffer, bufferRange.Offset, resource, 0, bufferRange.Size);

			m_trackedAllocations.emplace_back(allocation);
		}

		template <typename T>
		void Copy(GPUBuffer& buffer, initializer_list<T> data, size_t offset = 0) { COPY(); }

		template <typename T>
		void Copy(GPUBuffer& buffer, span<const T> data, size_t offset = 0) { COPY(); }

		template <typename T>
		void Copy(GPUBuffer& buffer, const vector<T>& data, size_t offset = 0) { COPY(); }

		void SetRenderTarget(Texture& texture) {
			SetState(texture, D3D12_RESOURCE_STATE_RENDER_TARGET);
			const auto& descriptor = texture.GetRTVDescriptor().GetCPUHandle();
			(*this)->OMSetRenderTargets(1, &descriptor, FALSE, nullptr);
		}

		void Clear(Texture& texture, UINT16 mipLevel = 0) {
			SetState(texture, D3D12_RESOURCE_STATE_RENDER_TARGET);
			const auto& clearColor = texture.GetClearColor();
			(*this)->ClearRenderTargetView(texture.GetRTVDescriptor(mipLevel), reinterpret_cast<const float*>(&clearColor), 0, nullptr);
		}

		vector<uint64_t> BuildAccelerationStructures(span<const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS> inputs) {
			vector<uint64_t> IDs;
			m_deviceContext.AccelerationStructureManager->PopulateBuildCommandList(*this, data(inputs), size(inputs), IDs);
			m_deviceContext.AccelerationStructureManager->PopulateUAVBarriersCommandList(*this, IDs);
			m_deviceContext.AccelerationStructureManager->PopulateCompactionSizeCopiesCommandList(*this, IDs);

			for (size_t i = 0; const auto ID : IDs) {
				if (inputs[i++].Flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION) {
					m_compactAccelerationStructureIDs[1].emplace_back(ID);
				}
				else m_builtAccelerationStructureIDs.emplace_back(ID);
			}

			return IDs;
		}

		void UpdateAccelerationStructures(
			span<const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS> inputs,
			const vector<uint64_t>& IDs
		) {
			m_deviceContext.AccelerationStructureManager->PopulateUpdateCommandList(*this, data(inputs), size(inputs), IDs);
			m_deviceContext.AccelerationStructureManager->PopulateUAVBarriersCommandList(*this, IDs);
		}

		void CompactAccelerationStructures() {
			if (!empty(m_compactAccelerationStructureIDs[0])) {
				m_deviceContext.AccelerationStructureManager->PopulateCompactionCommandList(*this, m_compactAccelerationStructureIDs[0]);
				m_builtAccelerationStructureIDs.append_range(m_compactAccelerationStructureIDs[0]);
				m_compactAccelerationStructureIDs[0].clear();
			}
		}

	private:
		const DeviceContext& m_deviceContext;

		ComPtr<ID3D12CommandAllocator> m_commandAllocator;
		ComPtr<ID3D12GraphicsCommandList4> m_commandList;

		ComPtr<Pool> m_pool;

		UINT m_fenceValue{};
		ComPtr<ID3D12Fence> m_fence;
		Event m_fenceEvent;

		unordered_set<GPUResource*> m_trackedResources;
		vector<ComPtr<Allocation>> m_trackedAllocations;

		vector<uint64_t> m_builtAccelerationStructureIDs, m_compactAccelerationStructureIDs[2];
	};
}
