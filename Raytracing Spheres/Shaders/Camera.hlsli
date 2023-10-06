#pragma once

#include "STL.hlsli"

struct Camera {
	bool IsNormalizedFarDepth0;
	float3 Position;
	float3 RightDirection;
	float _1;
	float3 UpDirection;
	float _2;
	float3 ForwardDirection;
	float ApertureRadius;
	float NearDepth, FarDepth;
	float2 PixelJitter;
	float4x4 PreviousWorldToView, PreviousViewToProjection, PreviousWorldToProjection, PreviousViewToWorld, WorldToProjection;

	RayDesc GeneratePinholeRay(float2 NDC) {
		RayDesc rayDesc;
		rayDesc.Origin = Position;
		rayDesc.Direction = normalize(NDC.x * RightDirection + NDC.y * UpDirection + ForwardDirection);
		const float invCos = 1 / dot(normalize(ForwardDirection), rayDesc.Direction);
		rayDesc.TMin = NearDepth * invCos;
		rayDesc.TMax = FarDepth * invCos;
		return rayDesc;
	}

	RayDesc GenerateThinLensRay(float2 NDC) {
		const float2 value = STL::ImportanceSampling::Uniform::GetRay(STL::Rng::Hash::GetFloat2()).xy;
		const float3 offset = (normalize(RightDirection) * value.x + normalize(UpDirection) * value.y) * ApertureRadius;
		RayDesc rayDesc;
		rayDesc.Origin = Position + offset;
		rayDesc.Direction = normalize(NDC.x * RightDirection + NDC.y * UpDirection + ForwardDirection - offset);
		const float invCos = 1 / dot(normalize(ForwardDirection), rayDesc.Direction);
		rayDesc.TMin = NearDepth * invCos;
		rayDesc.TMax = FarDepth * invCos;
		return rayDesc;
	}
};
