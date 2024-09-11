module;

#include <DirectXMath.h>

#include "ml.h"
#include "ml.hlsli"

export module Vertex;

using namespace DirectX;
using namespace Packing;

export {
	struct VertexDesc { uint32_t Stride{}, NormalOffset = ~0u, TextureCoordinateOffset = ~0u, TangentOffset = ~0u; };

	struct VertexPositionNormalTexture {
		XMFLOAT3 Position;
		uint32_t TextureCoordinate;
		XMFLOAT2 Normal, _{};

		VertexPositionNormalTexture(const XMFLOAT3& position, const XMFLOAT3& normal, XMFLOAT2 textureCoordinate) :
			Position(position),
			TextureCoordinate(reinterpret_cast<const uint32_t&>(float2_to_float16_t2(reinterpret_cast<const float2&>(textureCoordinate)))),
			Normal(reinterpret_cast<const XMFLOAT2&>(EncodeUnitVector(reinterpret_cast<const float3&>(normal), true))) {}
	};
}
