module;

#include "directxtk12/ResourceUploadBatch.h"
#include "directxtk12/VertexTypes.h"

#include <memory>
#include <span>
#include <string>

export module Model;

import DescriptorHeap;
import GPUBuffer;

using namespace DirectX;
using namespace std;

export struct Mesh {
	using VertexType = VertexPositionNormalTexture;
	using IndexType = UINT16;

	string Name;

	shared_ptr<RWStructuredBuffer<VertexType>> Vertices;
	shared_ptr<RWStructuredBuffer<IndexType>> Indices;

	struct { UINT Vertices = ~0u, Indices = ~0u; } DescriptorHeapIndices;

	static shared_ptr<Mesh> Create(span<const VertexType> vertices, span<const IndexType> indices, ID3D12Device* pDevice, ResourceUploadBatch& resourceUploadBatch, DescriptorHeapEx& descriptorHeap, _Inout_ UINT& descriptorHeapIndex) {
		const auto CreateBuffer = [&]<typename T>(shared_ptr<T>&buffer, const auto & data, D3D12_RESOURCE_STATES afterState, UINT & srvDescriptorHeapIndex) {
			buffer = make_shared<T>(pDevice, resourceUploadBatch, data, afterState);
			descriptorHeapIndex = descriptorHeap.Allocate(1, descriptorHeapIndex);
			srvDescriptorHeapIndex = descriptorHeapIndex - 1;
			buffer->CreateShaderResourceView(descriptorHeap.GetCpuHandle(srvDescriptorHeapIndex));
		};
		const auto mesh = make_shared<Mesh>();
		CreateBuffer(mesh->Vertices, vertices, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, mesh->DescriptorHeapIndices.Vertices);
		CreateBuffer(mesh->Indices, indices, D3D12_RESOURCE_STATE_INDEX_BUFFER, mesh->DescriptorHeapIndices.Indices);
		return mesh;
	}
};
