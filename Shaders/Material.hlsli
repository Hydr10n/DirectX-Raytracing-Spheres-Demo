#pragma once

enum class AlphaMode
{
	Opaque, Mask, Blend
};

struct Material
{
	float4 BaseColor;
	float3 EmissiveColor;
	float EmissiveStrength, Metallic, Roughness, Transmission, IOR;
	AlphaMode AlphaMode;
	float AlphaCutoff;
	uint2 _;

	float3 GetEmission()
	{
		return EmissiveColor * EmissiveStrength;
	}
};
