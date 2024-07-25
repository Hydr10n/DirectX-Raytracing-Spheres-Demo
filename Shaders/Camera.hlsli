#pragma once

#include "ml.hlsli"

struct Camera
{
	bool IsNormalizedDepthReversed;
	float3 PreviousPosition, Position;
	float _;
	float3 RightDirection;
	float _1;
	float3 UpDirection;
	float _2;
	float3 ForwardDirection;
	float ApertureRadius, NearDepth, FarDepth;
	float2 Jitter;
	float4x4 PreviousWorldToView, PreviousViewToProjection, PreviousWorldToProjection, PreviousProjectionToView, PreviousViewToWorld, WorldToProjection;

	float3 GenerateRayDirection(float2 NDC)
	{
		return NDC.x * RightDirection + NDC.y * UpDirection + ForwardDirection;
	}

	RayDesc GeneratePinholeRay(float2 NDC)
	{
		RayDesc rayDesc;
		rayDesc.Origin = Position;
		rayDesc.Direction = normalize(GenerateRayDirection(NDC));
		const float invCos = 1 / dot(normalize(ForwardDirection), rayDesc.Direction);
		rayDesc.TMin = NearDepth * invCos;
		rayDesc.TMax = FarDepth * invCos;
		return rayDesc;
	}

	RayDesc GenerateThinLensRay(float2 NDC)
	{
		const float2 value = ImportanceSampling::Uniform::GetRay(Rng::Hash::GetFloat2()).xy;
		const float3 offset = (normalize(RightDirection) * value.x + normalize(UpDirection) * value.y) * ApertureRadius;
		RayDesc rayDesc;
		rayDesc.Origin = Position + offset;
		rayDesc.Direction = normalize(GenerateRayDirection(NDC) - offset);
		const float invCos = 1 / dot(normalize(ForwardDirection), rayDesc.Direction);
		rayDesc.TMin = NearDepth * invCos;
		rayDesc.TMax = FarDepth * invCos;
		return rayDesc;
	}

	float3 ReconstructWorldPosition(float2 NDC, float linearDepth, bool isPrevious)
	{
		if (isPrevious)
		{
			float4 projection = Geometry::ProjectiveTransform(PreviousProjectionToView, float4(NDC, 0.5f, 1));
			projection.xy /= projection.z;
			projection.zw = 1;
			projection.xyz *= linearDepth;
			return Geometry::AffineTransform(PreviousViewToWorld, projection);
		}
		const float3 direction = normalize(NDC.x * RightDirection + NDC.y * UpDirection + ForwardDirection);
		return Position + direction * linearDepth / dot(normalize(ForwardDirection), direction);
	}
};
