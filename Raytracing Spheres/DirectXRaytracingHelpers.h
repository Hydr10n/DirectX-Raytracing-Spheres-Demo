#pragma once

#include "pch.h"

namespace DirectX {
	inline HRESULT CreateBuffer(
		ID3D12Device* pDevice,
		UINT64 size,
		D3D12_RESOURCE_STATES initialState,
		ID3D12Resource** ppBuffer,
		D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE,
		const D3D12_HEAP_PROPERTIES& heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT)
	) {
		const auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(size, flags);
		return pDevice->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &resourceDesc, initialState, nullptr, IID_PPV_ARGS(ppBuffer));
	}

	inline HRESULT CreateUploadBuffer(ID3D12Device* pDevice, UINT64 size, ID3D12Resource** ppBuffer, const void* pData = nullptr) {
		PVOID pMappedData;
		auto ret = CreateBuffer(pDevice, size, D3D12_RESOURCE_STATE_GENERIC_READ, ppBuffer, D3D12_RESOURCE_FLAG_NONE, CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD));
		if (SUCCEEDED(ret) && pData != nullptr && SUCCEEDED(ret = (*ppBuffer)->Map(0, nullptr, &pMappedData))) {
			memcpy(pMappedData, pData, static_cast<size_t>(size));
			(*ppBuffer)->Unmap(0, nullptr);
		}
		return ret;
	}

	namespace RaytracingHelpers {
		struct AccelerationStructureBuffers {
			Microsoft::WRL::ComPtr<ID3D12Resource> Scratch, Result, InstanceDesc;

			AccelerationStructureBuffers() = default;

			AccelerationStructureBuffers(
				ID3D12Device* pDevice,
				UINT64 scratchSize, UINT64 resultSize,
				UINT64 instanceDescsSize = 0
			) noexcept(false) {
				DX::ThrowIfFailed(CreateBuffer(pDevice, scratchSize, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &Scratch, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS));
				DX::ThrowIfFailed(CreateBuffer(pDevice, resultSize, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, &Result, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS));
				if (instanceDescsSize) DX::ThrowIfFailed(CreateUploadBuffer(pDevice, instanceDescsSize, &InstanceDesc));
			}
		};

		template <class Vertex, class Index>
		class Triangles {
			static_assert(std::is_same<Index, UINT16>() || std::is_same<Index, UINT32>(), "Unsupported index format");

		public:
			Triangles(
				ID3D12Device* pDevice,
				const std::vector<Vertex>& vertices, const std::vector<Index>& indices,
				D3D12_RAYTRACING_GEOMETRY_FLAGS flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE,
				D3D12_GPU_VIRTUAL_ADDRESS transform3x4 = NULL,
				DXGI_FORMAT vertexFormat = DXGI_FORMAT_R32G32B32_FLOAT
			) noexcept(false) : m_device(pDevice) {
				const auto verticesSize = vertices.size(), indicesSize = indices.size();

				DX::ThrowIfFailed(CreateUploadBuffer(pDevice, sizeof(Vertex) * verticesSize, &m_vertexBuffer, vertices.data()));

				DX::ThrowIfFailed(CreateUploadBuffer(pDevice, sizeof(Index) * indicesSize, &m_indexBuffer, indices.data()));

				m_geometryDesc = {
					.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES,
					.Flags = flags,
					.Triangles = {
						.Transform3x4 = transform3x4,
						.IndexFormat = std::is_same<Index, UINT16>() ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT,
						.VertexFormat = vertexFormat,
						.IndexCount = static_cast<UINT>(indicesSize),
						.VertexCount = static_cast<UINT>(verticesSize),
						.IndexBuffer = m_indexBuffer->GetGPUVirtualAddress(),
						.VertexBuffer = {
							.StartAddress = m_vertexBuffer->GetGPUVirtualAddress(),
							.StrideInBytes = sizeof(Vertex),
						}
					}
				};
			}

			ID3D12Resource* GetVertexBuffer() const { return m_vertexBuffer.Get(); }

			ID3D12Resource* GetIndexBuffer() const { return m_indexBuffer.Get(); }

			const D3D12_RAYTRACING_GEOMETRY_DESC& GetGeometryDesc() const { return m_geometryDesc; }

			void CreateShaderResourceViews(D3D12_CPU_DESCRIPTOR_HANDLE vertexDescriptor, D3D12_CPU_DESCRIPTOR_HANDLE indexDescriptor) const {
				D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{
					.ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
					.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING
				};

				srvDesc.Format = DXGI_FORMAT_UNKNOWN;
				srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
				srvDesc.Buffer.NumElements = m_geometryDesc.Triangles.VertexCount;
				srvDesc.Buffer.StructureByteStride = sizeof(Vertex);
				m_device->CreateShaderResourceView(m_vertexBuffer.Get(), &srvDesc, vertexDescriptor);

				srvDesc.Format = DXGI_FORMAT_R32_TYPELESS;
				srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
				srvDesc.Buffer.NumElements = std::is_same<Index, UINT16>() ? (m_geometryDesc.Triangles.IndexCount + 1) >> 1 : m_geometryDesc.Triangles.IndexCount;
				srvDesc.Buffer.StructureByteStride = 0;
				m_device->CreateShaderResourceView(m_indexBuffer.Get(), &srvDesc, indexDescriptor);
			}

		private:
			ID3D12Device* m_device;

			Microsoft::WRL::ComPtr<ID3D12Resource> m_vertexBuffer, m_indexBuffer;

			D3D12_RAYTRACING_GEOMETRY_DESC m_geometryDesc;
		};
	}
}
