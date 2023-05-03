module;

#include <string>

#include <span>

#include <memory>

#include "directxtk12/VertexTypes.h"

export module Model;

import DirectX.DescriptorHeap;
import DirectX.RaytracingHelpers;

using namespace DirectX;
using namespace DirectX::RaytracingHelpers;
using namespace std;

export struct ModelMesh {
	string Name;

	unique_ptr<TriangleMesh<VertexPositionNormalTexture, UINT16>> TriangleMesh;
	struct { UINT Vertices = ~0u, Indices = ~0u; } DescriptorHeapIndices;

	static shared_ptr<ModelMesh> Create(span<const VertexPositionNormalTexture> vertices, span<const UINT16> indices, ID3D12Device* pDevice, DescriptorHeapEx& descriptorHeap, _Inout_ UINT& descriptorHeapIndex) {
		auto modelMesh = make_shared<ModelMesh>();
		descriptorHeapIndex = descriptorHeap.Allocate(2, descriptorHeapIndex);
		modelMesh->DescriptorHeapIndices = { .Vertices = descriptorHeapIndex - 2, .Indices = descriptorHeapIndex - 1 };
		modelMesh->TriangleMesh = make_unique<::TriangleMesh<VertexPositionNormalTexture, UINT16>>(pDevice, vertices, indices);
		modelMesh->TriangleMesh->CreateShaderResourceViews(descriptorHeap.GetCpuHandle(modelMesh->DescriptorHeapIndices.Vertices), descriptorHeap.GetCpuHandle(modelMesh->DescriptorHeapIndices.Indices));
		return modelMesh;
	}
};
