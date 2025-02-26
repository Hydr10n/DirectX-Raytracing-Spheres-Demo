#pragma once

#include "ml.hlsli"

struct SurfaceVectors
{
	float IsFront;
	float3 FrontGeometricNormal, ShadingNormal;
	float3x3 ShadingBasis;

	void Initialize(bool isFront, float3 geometricNormal, float3 shadingNormal)
	{
		IsFront = isFront;
		FrontGeometricNormal = IsFront ? geometricNormal : -geometricNormal;
		ShadingNormal = shadingNormal;
		ShadingBasis = Geometry::GetBasis(shadingNormal);
	}
};
