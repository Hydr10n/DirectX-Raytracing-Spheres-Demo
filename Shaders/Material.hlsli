#pragma once

enum class AlphaMode
{
	Opaque, Blend, Mask
};

static const float MinRoughness = 5e-2f;

struct Material
{
	float4 BaseColor;
	float3 EmissiveColor;
	float Metallic, Roughness, Transmission, IOR;
	AlphaMode AlphaMode;
	float AlphaThreshold;
	float3 _;
};
