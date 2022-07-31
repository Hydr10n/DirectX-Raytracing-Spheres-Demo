module;
#include "pch.h"

export module DirectX.RaytracingHelpers;

using namespace DX;
using namespace Microsoft::WRL;
using namespace std;

export namespace DirectX::RaytracingHelpers {
	struct AccelerationStructureBuffers {
		ComPtr<ID3D12Resource> Scratch, Result, InstanceDesc;

		AccelerationStructureBuffers() = default;

		AccelerationStructureBuffers(ID3D12Device* pDevice, UINT64 scratchSize, UINT64 resultSize, UINT64 instanceDescsSize) noexcept(false) {
			const auto CreateBuffer = [&](auto size, auto initialState, auto& buffer, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_HEAP_TYPE type = D3D12_HEAP_TYPE_DEFAULT) {
				const D3D12_HEAP_PROPERTIES heapProperties(type);
				const auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(size, flags);
				ThrowIfFailed(pDevice->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &resourceDesc, initialState, nullptr, IID_PPV_ARGS(&buffer)));
			};

			CreateBuffer(scratchSize, D3D12_RESOURCE_STATE_COMMON, Scratch);

			CreateBuffer(resultSize, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, Result);

			if (instanceDescsSize) {
				CreateBuffer(instanceDescsSize, D3D12_RESOURCE_STATE_GENERIC_READ, InstanceDesc, D3D12_RESOURCE_FLAG_NONE, D3D12_HEAP_TYPE_UPLOAD);
			}
		}
	};

	template <typename Vertex, typename Index> requires same_as<Index, UINT16> || same_as<Index, UINT32>
	class TriangleMesh {
	public:
		using VertexType = Vertex;
		using IndexType = Index;

		TriangleMesh(
			ID3D12Device* pDevice,
			const vector<Vertex>& vertices, const vector<Index>& indices,
			D3D12_RAYTRACING_GEOMETRY_FLAGS flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE,
			D3D12_GPU_VIRTUAL_ADDRESS transform3x4 = NULL,
			DXGI_FORMAT vertexFormat = DXGI_FORMAT_R32G32B32_FLOAT
		) noexcept(false) : m_device(pDevice) {
			const auto CreateBuffer = [&](const auto& data, auto afterState, auto& buffer) {
				const auto size = data.size() * sizeof(data[0]);

				const D3D12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE_UPLOAD);
				const auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(size, D3D12_RESOURCE_FLAG_NONE);
				ThrowIfFailed(pDevice->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &resourceDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&buffer)));

				PVOID pMappedData;
				ThrowIfFailed(buffer->Map(0, nullptr, &pMappedData));
				memcpy(pMappedData, data.data(), size);
				buffer->Unmap(0, nullptr);
			};

			CreateBuffer(vertices, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, m_vertexBuffer);

			CreateBuffer(indices, D3D12_RESOURCE_STATE_INDEX_BUFFER, m_indexBuffer);

			m_geometryDesc = {
				.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES,
				.Flags = flags,
				.Triangles{
					.Transform3x4 = transform3x4,
					.IndexFormat = is_same<Index, UINT16>() ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT,
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

			shaderResourceViewDesc.Buffer.NumElements = m_geometryDesc.Triangles.VertexCount;
			shaderResourceViewDesc.Buffer.StructureByteStride = sizeof(Vertex);
			m_device->CreateShaderResourceView(m_vertexBuffer.Get(), &shaderResourceViewDesc, vertexDescriptor);

			shaderResourceViewDesc.Buffer.NumElements = m_geometryDesc.Triangles.IndexCount;
			shaderResourceViewDesc.Buffer.StructureByteStride = sizeof(Index);
			m_device->CreateShaderResourceView(m_indexBuffer.Get(), &shaderResourceViewDesc, indexDescriptor);
		}

	private:
		ID3D12Device* m_device;

		ComPtr<ID3D12Resource> m_vertexBuffer, m_indexBuffer;

		D3D12_RAYTRACING_GEOMETRY_DESC m_geometryDesc;
	};
}
