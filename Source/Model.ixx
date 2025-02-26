module;

#include <memory>
#include <span>

#include <d3d12.h>

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

	using VertexType = VertexPositionNormalTangentTexture;
	using IndexType = uint16_t;
	shared_ptr<GPUBuffer> Vertices;
	shared_ptr<GPUBuffer> Indices;

	using DestroyEvent = CallbackList<void(Mesh*)>;
	DestroyEvent OnDestroyed;

	static constexpr VertexDesc GetVertexDesc() {
		return {
			.Stride = sizeof(VertexType),
			.AttributeOffsets{
				.Normal = offsetof(VertexType, Normal),
				.Tangent = offsetof(VertexType, Tangent),
				.TextureCoordinates{ offsetof(VertexType, TextureCoordinates[0]) }
			}
		};
	}

	~Mesh() { OnDestroyed(this); }

	static auto Create(const vector<VertexType>& vertices, const vector<IndexType>& indices, const DeviceContext& deviceContext, CommandList& commandList) {
		const auto CreateBuffer = [&]<typename T>(auto & buffer, const vector<T> &data, DXGI_FORMAT format) {
			buffer = GPUBuffer::CreateDefault<T>(deviceContext, size(data), format);
			buffer->CreateSRV(format == DXGI_FORMAT_UNKNOWN ? BufferSRVType::Raw : BufferSRVType::Typed);
			commandList.Copy(*buffer, data);
			commandList.SetState(*buffer, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		};
		const auto mesh = make_shared<Mesh>();
		CreateBuffer(mesh->Vertices, vertices, DXGI_FORMAT_UNKNOWN);
		CreateBuffer(mesh->Indices, indices, DXGI_FORMAT_R16_UINT);
		return mesh;
	}
};
