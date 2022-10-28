#pragma once

#include "Numbers.hlsli"

#include "Random.hlsli"

namespace Math {
	inline float3 CalculateTangent(float3 positions[3], float2 textureCoordinates[3]) {
		const float2 d0 = textureCoordinates[1] - textureCoordinates[0], d1 = textureCoordinates[2] - textureCoordinates[0];
		return normalize(((positions[1] - positions[0]) * d1.y - (positions[2] - positions[0]) * d0.y) / (d0.x * d1.y - d0.y * d1.x));
	}

	inline float3 CalculatePerpendicularVector(float3 v) {
		const float3 a = abs(v);
		const uint x = a.x - a.y < 0 && a.x - a.z < 0 ? 1 : 0, y = a.y - a.z < 0 ? 1 ^ x : 0, z = 1 ^ (x | y);
		return cross(v, float3(x, y, z));
	}

	inline float3 CosineSampleHemisphere(float3 N, inout Random random) {
		const float2 value = random.Float2();
		const float3 B = CalculatePerpendicularVector(N), T = cross(B, N);
		const float r = sqrt(value.x), phi = 2 * Numbers::Pi * value.y;
		return T * (r * cos(phi).x) + B * (r * sin(phi)) + N * sqrt(1 - value.x);
	}
}
