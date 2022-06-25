#pragma once

#include "pch.h"

#include "directxtk12/BufferHelpers.h"

namespace DirectX::RaytracingHelpers {
	struct AccelerationStructureBuffers {
		Microsoft::WRL::ComPtr<ID3D12Resource> Scratch, Result, InstanceDesc;

		AccelerationStructureBuffers() = default;

		AccelerationStructureBuffers(
			ID3D12Device* pDevice,
			UINT64 scratchSize, UINT64 resultSize,
			UINT64 instanceDescsSize = 0
		) noexcept(false) {
			const auto CreateBuffer = [&](UINT64 size, D3D12_RESOURCE_STATES initialState, ID3D12Resource** ppBuffer, D3D12_RESOURCE_FLAGS flags, const D3D12_HEAP_PROPERTIES& heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT)) {
				const auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(size, flags);
				DX::ThrowIfFailed(pDevice->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &resourceDesc, initialState, nullptr, IID_PPV_ARGS(ppBuffer)));
			};

			CreateBuffer(scratchSize, D3D12_RESOURCE_STATE_COMMON, &Scratch, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

			CreateBuffer(resultSize, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, &Result, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

			if (instanceDescsSize) {
				CreateBuffer(instanceDescsSize, D3D12_RESOURCE_STATE_GENERIC_READ, &InstanceDesc, D3D12_RESOURCE_FLAG_NONE, CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD));
			}
		}
	};

	template <typename Vertex, typename Index>
	class Mesh {
		static_assert(std::is_same<Index, UINT16>() || std::is_same<Index, UINT32>(), "Unsupported index format");

	public:
		Mesh(
			ID3D12Device* pDevice,
			ResourceUploadBatch& resourceUploadBatch,
			const std::vector<Vertex>& vertices, const std::vector<Index>& indices,
			D3D12_RAYTRACING_GEOMETRY_FLAGS flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE,
			D3D12_GPU_VIRTUAL_ADDRESS transform3x4 = NULL,
			DXGI_FORMAT vertexFormat = DXGI_FORMAT_R32G32B32_FLOAT
		) noexcept(false) : m_device(pDevice) {
			DX::ThrowIfFailed(CreateStaticBuffer(pDevice, resourceUploadBatch, vertices, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, &m_vertexBuffer));

			DX::ThrowIfFailed(CreateStaticBuffer(pDevice, resourceUploadBatch, indices, D3D12_RESOURCE_STATE_INDEX_BUFFER, &m_indexBuffer));

			m_geometryDesc = {
				.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES,
				.Flags = flags,
				.Triangles{
					.Transform3x4 = transform3x4,
					.IndexFormat = std::is_same<Index, UINT16>() ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT,
					.VertexFormat = vertexFormat,
					.IndexCount = static_cast<UINT>(indices.size()),
					.VertexCount = static_cast<UINT>(vertices.size()),
					.IndexBuffer = m_indexBuffer->GetGPUVirtualAddress(),
					.VertexBuffer{
						.StartAddress = m_vertexBuffer->GetGPUVirtualAddress(),
						.StrideInBytes = sizeof(Vertex)
					}
				}
			};
		}

		ID3D12Resource* GetVertexBuffer() const noexcept { return m_vertexBuffer.Get(); }

		ID3D12Resource* GetIndexBuffer() const noexcept { return m_indexBuffer.Get(); }

		const D3D12_RAYTRACING_GEOMETRY_DESC& GetGeometryDesc() const noexcept { return m_geometryDesc; }

		void CreateShaderResourceViews(D3D12_CPU_DESCRIPTOR_HANDLE vertexDescriptor, D3D12_CPU_DESCRIPTOR_HANDLE indexDescriptor) const {
			D3D12_SHADER_RESOURCE_VIEW_DESC shaderResourceViewDesc{
				.ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
				.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING
			};

			shaderResourceViewDesc.Buffer = {
				.NumElements = m_geometryDesc.Triangles.VertexCount,
				.StructureByteStride = sizeof(Vertex)
			};
			m_device->CreateShaderResourceView(m_vertexBuffer.Get(), &shaderResourceViewDesc, vertexDescriptor);

			shaderResourceViewDesc.Format = DXGI_FORMAT_R32_TYPELESS;
			shaderResourceViewDesc.Buffer = {
				.NumElements = std::is_same<Index, UINT16>() ? (m_geometryDesc.Triangles.IndexCount + 1) >> 1 : m_geometryDesc.Triangles.IndexCount,
				.Flags = D3D12_BUFFER_SRV_FLAG_RAW
			};
			m_device->CreateShaderResourceView(m_indexBuffer.Get(), &shaderResourceViewDesc, indexDescriptor);
		}

	private:
		ID3D12Device* m_device;

		Microsoft::WRL::ComPtr<ID3D12Resource> m_vertexBuffer, m_indexBuffer;

		D3D12_RAYTRACING_GEOMETRY_DESC m_geometryDesc;
	};
}
