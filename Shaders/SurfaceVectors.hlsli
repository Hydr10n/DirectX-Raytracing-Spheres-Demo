#pragma once

#include "ml.hlsli"

struct SurfaceVectors
{
	float3 FrontGeometricNormal, ShadingNormal;
	float3x3 ShadingBasis;

	void Initialize(bool isFront, float3 geometricNormal, float3 shadingNormal)
	{
		FrontGeometricNormal = isFront ? geometricNormal : -geometricNormal;
		ShadingNormal = shadingNormal;
		ShadingBasis = Geometry::GetBasis(shadingNormal);
	}
};
