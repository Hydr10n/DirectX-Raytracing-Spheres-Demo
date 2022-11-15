#pragma once

#include "Math.hlsli"

struct Camera {
	float3 Position;
	float _;
	float3 RightDirection;
	float _1;
	float3 UpDirection;
	float _2;
	float3 ForwardDirection;
	float ApertureRadius;
	float2 PixelJitter;
	float NearZ, FarZ;

	RayDesc GeneratePinholeRay(uint2 pixelCoordinate, uint2 pixelDimensions) {
		const float2 NDC = Math::CalculateNDC(pixelCoordinate, pixelDimensions, PixelJitter);
		RayDesc rayDesc;
		rayDesc.Origin = Position;
		rayDesc.Direction = normalize(NDC.x * RightDirection + NDC.y * UpDirection + ForwardDirection);
		const float invCos = 1 / dot(normalize(ForwardDirection), rayDesc.Direction);
		rayDesc.TMin = NearZ * invCos;
		rayDesc.TMax = FarZ * invCos;
		return rayDesc;
	}

	RayDesc GenerateThinLensRay(uint2 pixelCoordinate, uint2 pixelDimensions, inout Random random) {
		const float2 NDC = Math::CalculateNDC(pixelCoordinate, pixelDimensions, PixelJitter), value = Math::SampleDisk(random);
		const float3 offset = (normalize(RightDirection) * value.x + normalize(UpDirection) * value.y) * ApertureRadius;
		RayDesc rayDesc;
		rayDesc.Origin = Position + offset;
		rayDesc.Direction = normalize(NDC.x * RightDirection + NDC.y * UpDirection + ForwardDirection - offset);
		const float invCos = 1 / dot(normalize(ForwardDirection), rayDesc.Direction);
		rayDesc.TMin = NearZ * invCos;
		rayDesc.TMax = FarZ * invCos;
		return rayDesc;
	}
};
