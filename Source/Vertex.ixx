module;

#include <DirectXMath.h>

export module Vertex;

using namespace DirectX;

export {
	struct VertexDesc { uint32_t Stride{}, NormalOffset = ~0u, TextureCoordinateOffset = ~0u, TangentOffset = ~0u; };

	struct VertexPositionNormalTexture {
		XMFLOAT3 Position;
		uint32_t TextureCoordinate;
		XMFLOAT2 Normal, _;
	};
}
