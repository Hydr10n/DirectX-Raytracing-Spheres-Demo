module;

#include "directxtk12/ResourceUploadBatch.h"
#include "directxtk12/VertexTypes.h"

#include <memory>
#include <span>
#include <string>

export module Model;

import DescriptorHeap;
import GPUBuffer;
import Vertex;

using namespace DirectX;
using namespace std;

export struct Mesh {
	using VertexType = VertexPositionNormalTexture;
	using IndexType = UINT16;

	string Name;

	shared_ptr<DefaultBuffer<VertexType>> Vertices;
	shared_ptr<DefaultBuffer<IndexType>> Indices;

	struct { UINT Vertices = ~0u, Indices = ~0u; } DescriptorHeapIndices;

	static constexpr VertexDesc VertexDesc{
		.Stride = sizeof(VertexType),
		.Offsets{
			.Normal = offsetof(VertexType, normal),
			.TextureCoordinate = offsetof(VertexType, textureCoordinate)
		}
	};

	static shared_ptr<Mesh> Create(span<const VertexType> vertices, span<const IndexType> indices, ID3D12Device* pDevice, ResourceUploadBatch& resourceUploadBatch, DescriptorHeapEx& descriptorHeap, _Inout_ UINT& descriptorHeapIndex) {
		const auto CreateBuffer = [&]<typename T>(shared_ptr<T>&buffer, const auto & data, D3D12_RESOURCE_STATES afterState, UINT & srvDescriptorHeapIndex, bool isVertex) {
			buffer = make_shared<T>(pDevice, resourceUploadBatch, data, afterState);
			descriptorHeapIndex = descriptorHeap.Allocate(1, descriptorHeapIndex);
			srvDescriptorHeapIndex = descriptorHeapIndex - 1;
			if (isVertex) buffer->CreateRawSRV(descriptorHeap.GetCpuHandle(srvDescriptorHeapIndex));
			else buffer->CreateStructuredSRV(descriptorHeap.GetCpuHandle(srvDescriptorHeapIndex));
		};
		const auto mesh = make_shared<Mesh>();
		CreateBuffer(mesh->Vertices, vertices, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, mesh->DescriptorHeapIndices.Vertices, true);
		CreateBuffer(mesh->Indices, indices, D3D12_RESOURCE_STATE_INDEX_BUFFER, mesh->DescriptorHeapIndices.Indices, false);
		return mesh;
	}
};
