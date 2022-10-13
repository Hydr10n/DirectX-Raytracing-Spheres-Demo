#pragma once

#include "Ray.hlsli"

struct Camera {
	float3 Position;
	float _;
	float4x4 ProjectionToWorld;

	Ray GenerateRay(uint2 pixelCoordinate, uint2 dimension, float2 offset = 0.5f) {
		const float2 NDC = (pixelCoordinate + offset) / dimension * float2(2, -2) + float2(-1, 1);
		const float4 world = mul(float4(NDC, 0, 1), ProjectionToWorld);
		const Ray ray = { Position, normalize(world.xyz / world.w - Position) };
		return ray;
	}
};
