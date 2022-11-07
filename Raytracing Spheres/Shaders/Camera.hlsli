#pragma once

#include "Ray.hlsli"

struct Camera {
	float3 Position;
	float _;
	float2 Jitter;
	float2 _1;
	float4x4 ProjectionToWorld;

	Ray GenerateRay(uint2 pixelCoordinate, uint2 dimensions) {
		const float2 NDC = (pixelCoordinate + 0.5f + Jitter) / dimensions * float2(2, -2) + float2(-1, 1);
		const float4 world = mul(float4(NDC, 0, 1), ProjectionToWorld);
		const Ray ray = { Position, normalize(world.xyz / world.w - Position) };
		return ray;
	}
};
