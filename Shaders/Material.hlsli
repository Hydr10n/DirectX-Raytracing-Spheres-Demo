#pragma once

enum class AlphaMode
{
	Opaque, Blend, Mask
};

struct Material
{
	float4 BaseColor;
	float3 EmissiveColor;
	float Metallic, Roughness, Transmission, IOR;
	AlphaMode AlphaMode;
	float AlphaThreshold;
	bool HasTexture;
	float2 _;
};
