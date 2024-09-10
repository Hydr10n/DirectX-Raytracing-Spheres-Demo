module;

#include <memory>
#include <span>

#include "directxtk12/VertexTypes.h"
#include "directxtk12/ResourceUploadBatch.h"

#include "ml.h"
#include "ml.hlsli"

export module Model;

import CommandList;
import DeviceContext;
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

	shared_ptr<GPUBuffer> Vertices;
	shared_ptr<GPUBuffer> Indices;

	inline static Event<const Mesh*> DeleteEvent;

	static constexpr VertexDesc GetVertexDesc() {
		return {
			.Stride = sizeof(VertexType),
			.NormalOffset = offsetof(VertexType, Normal),
			.TextureCoordinateOffset = offsetof(VertexType, TextureCoordinate)
		};
	}

	~Mesh() { DeleteEvent.Raise(this); }

	static auto Create(const vector<DirectX::VertexPositionNormalTexture>& vertices, const vector<IndexType>& indices, const DeviceContext& deviceContext, CommandList& commandList) {
		vector<VertexType> newVertices;
		newVertices.reserve(vertices.size());
		for (const auto vertex : vertices) {
			newVertices.emplace_back(vertex.position, reinterpret_cast<const uint32_t&>(float2_to_float16_t2(reinterpret_cast<const float2&>(vertex.textureCoordinate))), reinterpret_cast<const XMFLOAT2&>(EncodeUnitVector(reinterpret_cast<const float3&>(vertex.normal), true)));
		}
		const auto CreateBuffer = [&]<typename T>(auto & buffer, const vector<T> &data, D3D12_RESOURCE_STATES afterState, bool isStructuredSRV) {
			buffer = GPUBuffer::CreateDefault<T>(deviceContext, size(data));
			buffer->CreateSRV(isStructuredSRV ? BufferSRVType::Structured : BufferSRVType::Raw);
			commandList.Write(*buffer, data);
			commandList.SetState(*buffer, afterState);
		};
		const auto mesh = make_shared<Mesh>();
		CreateBuffer(mesh->Vertices, newVertices, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, false);
		CreateBuffer(mesh->Indices, indices, D3D12_RESOURCE_STATE_INDEX_BUFFER, true);
		return mesh;
	}
};
