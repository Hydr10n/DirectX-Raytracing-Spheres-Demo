#pragma once

enum class AlphaMode
{
	Opaque, Blend, Mask
};

struct Material
{
	float4 BaseColor;
	float3 EmissiveColor;
	float EmissiveIntensity, Metallic, Roughness, Transmission, IOR;
	AlphaMode AlphaMode;
	float AlphaCutoff;
	bool HasTexture;
	uint _;

	float3 GetEmission()
	{
		return EmissiveColor * EmissiveIntensity;
	}
};
