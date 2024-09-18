module;

#include <memory>
#include <span>

#include "directxtk12/VertexTypes.h"
#include "directxtk12/ResourceUploadBatch.h"

#include "eventpp/callbacklist.h"

export module Model;

import CommandList;
import DeviceContext;
import GPUBuffer;
import Vertex;

using namespace DirectX;
using namespace eventpp;
using namespace std;

export struct Mesh {
	string Name;

	using VertexType = ::VertexPositionNormalTexture;
	using IndexType = UINT16;
	shared_ptr<GPUBuffer> Vertices;
	shared_ptr<GPUBuffer> Indices;

	using DestroyEvent = CallbackList<void(Mesh*)>;
	DestroyEvent OnDestroyed;

	static constexpr VertexDesc GetVertexDesc() {
		return {
			.Stride = sizeof(VertexType),
			.NormalOffset = offsetof(VertexType, Normal),
			.TextureCoordinateOffset = offsetof(VertexType, TextureCoordinate)
		};
	}

	~Mesh() { OnDestroyed(this); }

	static auto Create(const vector<DirectX::VertexPositionNormalTexture>& vertices, const vector<IndexType>& indices, const DeviceContext& deviceContext, CommandList& commandList) {
		vector<VertexType> newVertices;
		newVertices.reserve(vertices.size());
		for (const auto& [Position, Normal, TextureCoordinate] : vertices) {
			newVertices.emplace_back(::VertexPositionNormalTexture(Position, Normal, TextureCoordinate));
		}
		const auto CreateBuffer = [&]<typename T>(auto & buffer, const vector<T> &data, D3D12_RESOURCE_STATES afterState, bool isStructuredSRV) {
			buffer = GPUBuffer::CreateDefault<T>(deviceContext, size(data));
			buffer->CreateSRV(isStructuredSRV ? BufferSRVType::Structured : BufferSRVType::Raw);
			commandList.Copy(*buffer, data);
			commandList.SetState(*buffer, afterState);
		};
		const auto mesh = make_shared<Mesh>();
		CreateBuffer(mesh->Vertices, newVertices, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, false);
		CreateBuffer(mesh->Indices, indices, D3D12_RESOURCE_STATE_INDEX_BUFFER, true);
		return mesh;
	}
};
