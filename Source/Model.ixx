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
		const auto CreateBuffer = [&]<typename T>(auto & buffer, const vector<T> &data, DXGI_FORMAT format) {
			buffer = GPUBuffer::CreateDefault<T>(deviceContext, size(data), format);
			buffer->CreateSRV(format == DXGI_FORMAT_UNKNOWN ? BufferSRVType::Raw : BufferSRVType::Typed);
			commandList.Copy(*buffer, data);
			commandList.SetState(*buffer, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		};
		const auto mesh = make_shared<Mesh>();
		CreateBuffer(mesh->Vertices, newVertices, DXGI_FORMAT_UNKNOWN);
		CreateBuffer(mesh->Indices, indices, DXGI_FORMAT_R16_UINT);
		return mesh;
	}
};
