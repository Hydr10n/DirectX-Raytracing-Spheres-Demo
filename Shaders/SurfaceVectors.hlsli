#pragma once

#include "ml.hlsli"

struct SurfaceVectors
{
	float IsFront;
	float3 ShadingNormal, GeometricNormal, FrontNormal;
	float3x3 ShadingBasis;

	void Initialize(bool isFront, float3 shadingNormal, float3 geometricNormal)
	{
		IsFront = isFront;
		ShadingNormal = shadingNormal;
		GeometricNormal = geometricNormal;
		FrontNormal = IsFront ? geometricNormal : -geometricNormal;
		ShadingBasis = Geometry::GetBasis(shadingNormal);
	}
};
