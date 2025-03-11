module;

#include <DirectXPackedVector.h>

#include "ml.h"

export module Vertex;

import Math;

using namespace DirectX;
using namespace DirectX::PackedVector;
using namespace Math;
using namespace Packing;

namespace {
	auto EncodeUnitVector(const XMFLOAT3& value) {
		return reinterpret_cast<const int16_t3&>(uint2(
			float2_to_snorm_16_16(float2(value.x, value.y)),
			float2_to_snorm_16_16(float2(value.z, 0))
		));
	}

	auto EncodeTextureCoordinate(XMFLOAT2 value) {
		return reinterpret_cast<const XMHALF2&>(float2_to_float16_t2(reinterpret_cast<const float2&>(value)));
	}
}

export {
	struct VertexDesc {
		uint32_t Stride{};
		XMUINT3 _;
		struct {
			uint32_t Normal = ~0u, Tangent = ~0u, TextureCoordinates[2]{ ~0u, ~0u };
		} AttributeOffsets;
	};

	struct VertexPositionNormalTangentTexture {
		XMFLOAT3 Position;
		int16_t3 Normal, Tangent;
		XMHALF2 TextureCoordinates[2];

		void StoreNormal(const XMFLOAT3& value) { Normal = EncodeUnitVector(value); }

		void StoreTangent(const XMFLOAT3& value) { Tangent = EncodeUnitVector(value); }

		void StoreTextureCoordinate(XMFLOAT2 value, uint8_t index) {
			TextureCoordinates[index] = EncodeTextureCoordinate(value);
		}
	};
}
