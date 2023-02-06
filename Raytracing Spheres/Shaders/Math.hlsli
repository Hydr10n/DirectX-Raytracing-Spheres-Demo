#pragma once

#include "Numbers.hlsli"

#include "Random.hlsli"

namespace Math {
	inline float2 CalculateUV(float3 position, float4x4 worldToProjection) {
		const float4 projection = mul(worldToProjection, float4(position, 1));
		return projection.xy / projection.w * float2(0.5f, -0.5f) + 0.5f;
	}

	inline float2 CalculateUV(uint2 pixelCoordinate, uint2 pixelDimensions, float2 pixelJitter = 0) { return (pixelCoordinate + 0.5f + pixelJitter) / pixelDimensions; }

	inline float2 CalculateNDC(float2 UV) { return UV * float2(2, -2) + float2(-1, 1); }

	inline float3 CalculateTangent(float3 positions[3], float2 textureCoordinates[3]) {
		const float2 d0 = textureCoordinates[1] - textureCoordinates[0], d1 = textureCoordinates[2] - textureCoordinates[0];
		return normalize(((positions[1] - positions[0]) * d1.y - (positions[2] - positions[0]) * d0.y) / (d0.x * d1.y - d0.y * d1.x));
	}

	inline float3 CalculatePerpendicularVector(float3 v) {
		const float3 a = abs(v);
		const uint x = a.x - a.y < 0 && a.x - a.z < 0 ? 1 : 0, y = a.y - a.z < 0 ? 1 ^ x : 0, z = 1 ^ (x | y);
		return cross(v, float3(x, y, z));
	}

	inline float3 SampleCosineHemisphere(float3 N, inout Random random) {
		const float2 value = random.Float2();
		const float3 B = CalculatePerpendicularVector(N), T = cross(B, N);
		const float r = sqrt(value.x), phi = 2 * Numbers::Pi * value.y;
		return T * (r * cos(phi).x) + B * (r * sin(phi)) + N * sqrt(1 - value.x);
	}

	inline float2 SampleDisk(inout Random random) {
		const float2 value = random.Float2();
		const float radius = sqrt(value.x), phi = 2 * Numbers::Pi * value.y;
		return float2(radius * cos(phi), radius * sin(phi));
	}
}
