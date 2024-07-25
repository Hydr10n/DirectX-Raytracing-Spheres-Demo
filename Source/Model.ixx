module;

#include <memory>
#include <span>
#include <string>

#include "directxtk12/ResourceUploadBatch.h"
#include "directxtk12/VertexTypes.h"

#include "ml.h"
#include "ml.hlsli"

export module Model;

import DescriptorHeap;
import Event;
import GPUBuffer;
import Vertex;

using namespace DirectX;
using namespace Packing;
using namespace std;

export struct Mesh {
	using VertexType = ::VertexPositionNormalTexture;
	using IndexType = UINT16;

	string Name;

	shared_ptr<DefaultBuffer<VertexType>> Vertices;
	shared_ptr<DefaultBuffer<IndexType>> Indices;

	inline static Event<const Mesh*> DeleteEvent;

	static constexpr VertexDesc GetVertexDesc() {
		return {
			.Stride = sizeof(VertexType),
			.NormalOffset = offsetof(VertexType, Normal),
			.TextureCoordinateOffset = offsetof(VertexType, TextureCoordinate)
		};
	}

	~Mesh() { DeleteEvent.Raise(this); }

	static auto Create(span<const DirectX::VertexPositionNormalTexture> vertices, span<const IndexType> indices, ID3D12Device* pDevice, ResourceUploadBatch& resourceUploadBatch, DescriptorHeapEx& descriptorHeap, _Inout_ UINT& descriptorIndex) {
		vector<VertexType> newVertices;
		newVertices.reserve(vertices.size());
		for (const auto vertex : vertices) {
			newVertices.emplace_back(vertex.position, float2_to_float16_t2(reinterpret_cast<const float2&>(vertex.textureCoordinate)).xy, reinterpret_cast<const XMFLOAT2&>(EncodeUnitVector(reinterpret_cast<const float3&>(vertex.normal), true)));
		}
		const auto CreateBuffer = [&]<typename T>(shared_ptr<T>&buffer, const auto & data, D3D12_RESOURCE_STATES afterState, bool isStructuredSRV) {
			buffer = make_shared<T>(pDevice, resourceUploadBatch, data, afterState);
			descriptorIndex = descriptorHeap.Allocate(1, descriptorIndex);
			if (isStructuredSRV) buffer->CreateStructuredSRV(descriptorHeap, descriptorIndex - 1);
			else buffer->CreateRawSRV(descriptorHeap, descriptorIndex - 1);
		};
		const auto mesh = make_shared<Mesh>();
		CreateBuffer(mesh->Vertices, newVertices, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, false);
		CreateBuffer(mesh->Indices, indices, D3D12_RESOURCE_STATE_INDEX_BUFFER, true);
		return mesh;
	}
};
