#pragma once

enum class AlphaMode
{
	Opaque, Mask, Blend
};

struct Material
{
	float4 BaseColor;
	float EmissiveStrength;
	float3 EmissiveColor;
	float Metallic, Roughness, IOR, Transmission;
	AlphaMode AlphaMode;
	float AlphaCutoff;
	uint2 _;

	float3 GetEmission()
	{
		return EmissiveStrength * EmissiveColor;
	}
};

struct TextureMapType
{
	enum : uint
	{
		BaseColor,
		EmissiveColor,
		Metallic,
		Roughness,
		MetallicRoughness,
		Transmission,
		Normal,
		Count
	};
};

struct TextureMapInfo
{
	uint Descriptor, TextureCoordinateIndex;
	uint2 _;
};
