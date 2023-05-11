module;

#include "pch.h"

#include <string>

#include <span>

#include <memory>

#include "directxtk12/VertexTypes.h"

#include "directxtk12/DirectXHelpers.h"

#include "directxtk12/ResourceUploadBatch.h"
#include "directxtk12/BufferHelpers.h"

export module Model;

import DirectX.DescriptorHeap;

using namespace DirectX;
using namespace DX;
using namespace Microsoft::WRL;
using namespace std;

export struct Mesh {
	using VertexType = VertexPositionNormalTexture;
	using IndexType = UINT16;

	string Name;

	ComPtr<ID3D12Resource> Vertices, Indices;

	struct { UINT Vertices = ~0u, Indices = ~0u; } DescriptorHeapIndices;

	static shared_ptr<Mesh> Create(span<const VertexType> vertices, span<const IndexType> indices, ID3D12Device* pDevice, ResourceUploadBatch& resourceUploadBatch, DescriptorHeapEx& descriptorHeap, _Inout_ UINT& descriptorHeapIndex) {
		const auto CreateBuffer = [&](ComPtr<ID3D12Resource>& buffer, const auto& data, D3D12_RESOURCE_STATES initialState, UINT descriptorHeapIndex) {
			ThrowIfFailed(CreateStaticBuffer(pDevice, resourceUploadBatch, data, initialState, &buffer));
			CreateBufferShaderResourceView(pDevice, buffer.Get(), descriptorHeap.GetCpuHandle(descriptorHeapIndex), sizeof(data[0]));
		};

		const auto mesh = make_shared<Mesh>();
		descriptorHeapIndex = descriptorHeap.Allocate(2, descriptorHeapIndex);
		mesh->DescriptorHeapIndices = { .Vertices = descriptorHeapIndex - 1, .Indices = descriptorHeapIndex - 2 };
		CreateBuffer(mesh->Vertices, vertices, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, mesh->DescriptorHeapIndices.Vertices);
		CreateBuffer(mesh->Indices, indices, D3D12_RESOURCE_STATE_INDEX_BUFFER, mesh->DescriptorHeapIndices.Indices);
		return mesh;
	}
};
