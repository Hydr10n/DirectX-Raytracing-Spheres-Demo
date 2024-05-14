#pragma once

#include "STL.hlsli"

struct Camera {
	bool IsNormalizedDepthReversed;
	float3 Position, RightDirection;
	float _;
	float3 UpDirection;
	float _1;
	float3 ForwardDirection;
	float ApertureRadius, NearDepth, FarDepth;
	float2 Jitter;
	float4x4 PreviousWorldToView, PreviousViewToProjection, PreviousWorldToProjection, PreviousProjectionToView, PreviousViewToWorld, WorldToProjection;

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

	float3 ReconstructWorldPosition(float2 NDC, float linearDepth, bool isPrevious) {
		if (isPrevious) {
			float4 projection = STL::Geometry::ProjectiveTransform(PreviousProjectionToView, float4(NDC, 0.5f, 1));
			projection.xy /= projection.z;
			projection.zw = 1;
			projection.xyz *= linearDepth;
			return STL::Geometry::AffineTransform(PreviousViewToWorld, projection);
		}
		const float3 direction = normalize(NDC.x * RightDirection + NDC.y * UpDirection + ForwardDirection);
		return Position + direction * linearDepth / dot(normalize(ForwardDirection), direction);
	}
};
