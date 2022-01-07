#pragma once

#include "RaytracingBufferHelpers.h"

#include <vector>

namespace RaytracingHelpers {
	class Geometry {
	public:
		virtual ~Geometry() = 0 {}

		const D3D12_RAYTRACING_GEOMETRY_DESC& GetGeometryDesc() const { return m_geometryDesc; }

	protected:
		Geometry(D3D12_RAYTRACING_GEOMETRY_TYPE type, D3D12_RAYTRACING_GEOMETRY_FLAGS flags) : m_geometryDesc({ type, flags }) {}

		D3D12_RAYTRACING_GEOMETRY_DESC m_geometryDesc;
	};

	template <class Vertex, class Index>
	class Triangles : public Geometry {
		static_assert(std::is_class<Vertex>() && sizeof(Vertex) >= sizeof(float) * 3, "Unsupported vertex format");
		static_assert(std::is_same<Index, UINT16>() || std::is_same<Index, UINT32>(), "Unsupported index format");

	public:
		Triangles(
			ID3D12Device* pDevice,
			const std::vector<Vertex>& vertices, const std::vector<Index>& indices,
			D3D12_RAYTRACING_GEOMETRY_FLAGS flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE,
			D3D12_GPU_VIRTUAL_ADDRESS transform3x4 = NULL
		) noexcept(false) :
			m_device(pDevice),
			Geometry(D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES, flags) {
			const auto verticesSize = vertices.size(), indicesSize = indices.size();

			DX::ThrowIfFailed(CreateUploadBuffer(pDevice, sizeof(Vertex) * verticesSize, &m_vertexBuffer, vertices.data()));

			DX::ThrowIfFailed(CreateUploadBuffer(pDevice, sizeof(Index) * indicesSize, &m_indexBuffer, indices.data()));

			m_geometryDesc.Triangles = {
				.Transform3x4 = transform3x4,
				.IndexFormat = std::is_same<Index, UINT16>() ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT,
				.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT,
				.IndexCount = static_cast<UINT>(indicesSize),
				.VertexCount = static_cast<UINT>(verticesSize),
				.IndexBuffer = m_indexBuffer->GetGPUVirtualAddress(),
				.VertexBuffer = {
					.StartAddress = m_vertexBuffer->GetGPUVirtualAddress(),
					.StrideInBytes = sizeof(Vertex),
				}
			};
		}

		ID3D12Resource* GetVertexBuffer() const { return m_vertexBuffer.Get(); }

		ID3D12Resource* GetIndexBuffer() const { return m_indexBuffer.Get(); }

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
			srvDesc.Buffer.NumElements = m_geometryDesc.Triangles.IndexCount / sizeof(Index);
			srvDesc.Buffer.StructureByteStride = 0;
			m_device->CreateShaderResourceView(m_indexBuffer.Get(), &srvDesc, indexDescriptor);
		}

	private:
		ID3D12Device* m_device;

		Microsoft::WRL::ComPtr<ID3D12Resource> m_vertexBuffer, m_indexBuffer;
	};
}
