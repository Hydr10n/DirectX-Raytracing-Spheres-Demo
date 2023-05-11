module;

#include "pch.h"

#include "directxtk12/DirectXHelpers.h"

export module DirectX.RaytracingHelpers;

using namespace DX;
using namespace Microsoft::WRL;
using namespace std;

export namespace DirectX::RaytracingHelpers {
	struct AccelerationStructureBuffers {
		ComPtr<ID3D12Resource> Scratch, Result, InstanceDescs;

		AccelerationStructureBuffers() = default;

		AccelerationStructureBuffers(ID3D12Device* pDevice, UINT64 scratchSize, UINT64 resultSize, UINT64 instanceDescsSize) noexcept(false) {
			const auto CreateBuffer = [&](ComPtr<ID3D12Resource>& buffer, UINT64 size, D3D12_RESOURCE_STATES initialState, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_HEAP_TYPE type = D3D12_HEAP_TYPE_DEFAULT) {
				const CD3DX12_HEAP_PROPERTIES heapProperties(type);
				const auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(size, flags);
				ThrowIfFailed(pDevice->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &resourceDesc, initialState, nullptr, IID_PPV_ARGS(&buffer)));
			};

			CreateBuffer(Scratch, scratchSize, D3D12_RESOURCE_STATE_COMMON);

			CreateBuffer(Result, resultSize, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);

			if (instanceDescsSize) CreateBuffer(InstanceDescs, instanceDescsSize, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, D3D12_HEAP_TYPE_UPLOAD);
		}
	};

	template <typename T> requires same_as<T, D3D12_RAYTRACING_GEOMETRY_DESC> || same_as<T, D3D12_RAYTRACING_INSTANCE_DESC>
	class AccelerationStructure {
	public:
		AccelerationStructure(ID3D12Device5* pDevice, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE) :
			m_device(pDevice), m_flags(flags) {}

		ID3D12Resource* GetBuffer() const noexcept { return m_buffers.Result.Get(); }

		UINT GetDescCount() const noexcept { return m_descCount; }

		void Build(ID3D12GraphicsCommandList4* pCommandList, span<const T> descs, bool updateOnly) {
			constexpr auto IsBottom = is_same_v<T, D3D12_RAYTRACING_GEOMETRY_DESC>;

			m_descCount = static_cast<UINT>(size(descs));

			D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc;
			desc.Inputs = {
				.Flags = m_flags,
				.NumDescs = m_descCount
			};
			if constexpr (IsBottom) {
				desc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
				desc.Inputs.pGeometryDescs = data(descs);
			}
			else desc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;

			if (updateOnly && m_flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE && m_buffers.Result != nullptr) {
				desc.Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
				desc.SourceAccelerationStructureData = m_buffers.Result->GetGPUVirtualAddress();
			}
			else {
				D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
				m_device->GetRaytracingAccelerationStructurePrebuildInfo(&desc.Inputs, &info);

				m_buffers = AccelerationStructureBuffers(m_device, AlignUp(info.ScratchDataSizeInBytes, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT), AlignUp(info.ResultDataMaxSizeInBytes, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT), IsBottom ? 0 : AlignUp(sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * desc.Inputs.NumDescs, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT));

				desc.SourceAccelerationStructureData = NULL;
			}

			if constexpr (!IsBottom) {
				if (m_buffers.InstanceDescs != nullptr) {
					D3D12_RAYTRACING_INSTANCE_DESC* pInstanceDescs;
					ThrowIfFailed(m_buffers.InstanceDescs->Map(0, nullptr, reinterpret_cast<void**>(&pInstanceDescs)));
					copy(cbegin(descs), cend(descs), pInstanceDescs);
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

	using BottomLevelAccelerationStructure = AccelerationStructure<D3D12_RAYTRACING_GEOMETRY_DESC>;
	using TopLevelAccelerationStructure = AccelerationStructure<D3D12_RAYTRACING_INSTANCE_DESC>;

	template <typename Vertex, typename Index> requires same_as<Index, UINT16> || same_as<Index, UINT32>
	D3D12_RAYTRACING_GEOMETRY_DESC CreateGeometryDesc(
		ID3D12Resource * pVertexBuffer, ID3D12Resource * pIndexBuffer,
		D3D12_RAYTRACING_GEOMETRY_FLAGS flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE,
		D3D12_GPU_VIRTUAL_ADDRESS transform3x4 = NULL,
		DXGI_FORMAT vertexFormat = DXGI_FORMAT_R32G32B32_FLOAT
	) {
		const auto vertexCount = pVertexBuffer->GetDesc().Width / sizeof(Vertex), indexCount = pIndexBuffer->GetDesc().Width / sizeof(Index);
		if (vertexCount < 3) throw invalid_argument("Vertex count cannot be fewer than 3");
		if (indexCount < 3) throw invalid_argument("Index count cannot be fewer than 3");
		return D3D12_RAYTRACING_GEOMETRY_DESC{
			.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES,
			.Flags = flags,
			.Triangles = {
				.Transform3x4 = transform3x4,
				.IndexFormat = is_same_v<Index, UINT16> ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT,
				.VertexFormat = vertexFormat,
				.IndexCount = static_cast<UINT>(indexCount),
				.VertexCount = static_cast<UINT>(vertexCount),
				.IndexBuffer = pIndexBuffer->GetGPUVirtualAddress(),
				.VertexBuffer{
					.StartAddress = pVertexBuffer->GetGPUVirtualAddress(),
					.StrideInBytes = sizeof(Vertex)
				}
			}
		};
	}
}
