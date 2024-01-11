module;

#include "directx/d3dx12.h"

#include "directxtk12/DirectXHelpers.h"

#include <span>
#include <stdexcept>

export module RaytracingHelpers;

import ErrorHelpers;
import GPUBuffer;

using namespace ErrorHelpers;
using namespace Microsoft::WRL;
using namespace std;

export namespace DirectX::RaytracingHelpers {
	struct AccelerationStructureBuffers {
		ComPtr<ID3D12Resource> Scratch, Result, InstanceDescs;

		AccelerationStructureBuffers() = default;

		AccelerationStructureBuffers(ID3D12Device* pDevice, size_t scratchSize, size_t resultSize, size_t instanceDescCount) noexcept(false) {
			const auto CreateBuffer = [&](ComPtr<ID3D12Resource>& buffer, size_t size, D3D12_RESOURCE_STATES initialState, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_HEAP_TYPE type = D3D12_HEAP_TYPE_DEFAULT) {
				const CD3DX12_HEAP_PROPERTIES heapProperties(type);
				const auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(size, flags);
				ThrowIfFailed(pDevice->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &resourceDesc, initialState, nullptr, IID_PPV_ARGS(&buffer)));
			};
			CreateBuffer(Scratch, scratchSize, D3D12_RESOURCE_STATE_COMMON);
			CreateBuffer(Result, resultSize, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
			if (instanceDescCount) CreateBuffer(InstanceDescs, sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * instanceDescCount, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, D3D12_HEAP_TYPE_UPLOAD);
		}
	};

	template <bool IsTop>
	class AccelerationStructure {
	public:
		AccelerationStructure(ID3D12Device5* pDevice, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE) : m_device(pDevice), m_flags(flags) {}

		ID3D12Resource* GetBuffer() const noexcept { return m_buffers.Result.Get(); }

		UINT GetDescCount() const noexcept { return m_descCount; }

		void Build(ID3D12GraphicsCommandList4* pCommandList, span<const conditional_t<IsTop, D3D12_RAYTRACING_INSTANCE_DESC, D3D12_RAYTRACING_GEOMETRY_DESC>> descs, bool updateOnly) {
			m_descCount = static_cast<UINT>(size(descs));

			D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc;
			desc.Inputs = {
				.Flags = m_flags,
				.NumDescs = m_descCount
			};
			if constexpr (IsTop) desc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
			else {
				desc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
				desc.Inputs.pGeometryDescs = data(descs);
			}

			if (updateOnly && m_flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE && m_buffers.Result != nullptr) {
				desc.Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
				desc.SourceAccelerationStructureData = m_buffers.Result->GetGPUVirtualAddress();
			}
			else {
				D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
				m_device->GetRaytracingAccelerationStructurePrebuildInfo(&desc.Inputs, &info);

				m_buffers = AccelerationStructureBuffers(m_device, info.ScratchDataSizeInBytes, info.ResultDataMaxSizeInBytes, IsTop ? desc.Inputs.NumDescs : 0);

				desc.SourceAccelerationStructureData = NULL;
			}

			if constexpr (IsTop) {
				if (m_buffers.InstanceDescs != nullptr) {
					D3D12_RAYTRACING_INSTANCE_DESC* pInstanceDescs;
					ThrowIfFailed(m_buffers.InstanceDescs->Map(0, nullptr, reinterpret_cast<void**>(&pInstanceDescs)));
					ranges::copy(descs, pInstanceDescs);
					m_buffers.InstanceDescs->Unmap(0, nullptr);

					desc.Inputs.InstanceDescs = m_buffers.InstanceDescs->GetGPUVirtualAddress();
				}
			}

			desc.DestAccelerationStructureData = m_buffers.Result->GetGPUVirtualAddress();
			desc.ScratchAccelerationStructureData = m_buffers.Scratch->GetGPUVirtualAddress();
			pCommandList->BuildRaytracingAccelerationStructure(&desc, 0, nullptr);

			const auto uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(m_buffers.Result.Get());
			pCommandList->ResourceBarrier(1, &uavBarrier);
		}

	private:
		ID3D12Device5* m_device;

		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS m_flags;

		UINT m_descCount{};

		AccelerationStructureBuffers m_buffers;
	};

	using BottomLevelAccelerationStructure = AccelerationStructure<false>;
	using TopLevelAccelerationStructure = AccelerationStructure<true>;

	template <typename Vertex, D3D12_HEAP_TYPE VertexHeapType, typename Index, D3D12_HEAP_TYPE IndexHeapType> requires same_as<Index, UINT16> || same_as<Index, UINT32>
	D3D12_RAYTRACING_GEOMETRY_DESC CreateGeometryDesc(
		const GPUBuffer<Vertex, VertexHeapType>&vertices, const GPUBuffer<Index, IndexHeapType>&indices,
		D3D12_RAYTRACING_GEOMETRY_FLAGS flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE,
		D3D12_GPU_VIRTUAL_ADDRESS transform3x4 = NULL,
		DXGI_FORMAT vertexFormat = DXGI_FORMAT_R32G32B32_FLOAT
	) {
		const auto vertexCount = vertices.GetCount(), indexCount = indices.GetCount();
		if (indexCount % 3 != 0) throw_std_exception<invalid_argument>("Triangle index count must be divisible by 3");
		return {
			.Flags = flags,
			.Triangles{
				.Transform3x4 = transform3x4,
				.IndexFormat = is_same_v<Index, UINT16> ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT,
				.VertexFormat = vertexFormat,
				.IndexCount = static_cast<UINT>(indexCount),
				.VertexCount = static_cast<UINT>(vertexCount),
				.IndexBuffer = indices.GetResource()->GetGPUVirtualAddress(),
				.VertexBuffer{
					.StartAddress = vertices.GetResource()->GetGPUVirtualAddress(),
					.StrideInBytes = vertices.ItemSize
				}
			}
		};
	}
}
