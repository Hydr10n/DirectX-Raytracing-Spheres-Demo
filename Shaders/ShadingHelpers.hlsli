#pragma once

#include "Common.hlsli"

#include "Math.hlsli"

#include "MeshHelpers.hlsli"

#include "BxDF.hlsli"

float3 GetEnvironmentLightColor(SceneData sceneData, float3 worldRayDirection)
{
	const uint descriptor = sceneData.EnvironmentLightTextureDescriptor;
	if (descriptor != ~0u)
	{
		worldRayDirection = normalize(Geometry::RotateVector((float3x3)sceneData.EnvironmentLightTransform, worldRayDirection));
		if (sceneData.IsEnvironmentLightTextureCubeMap)
		{
			const TextureCube<float3> texture = ResourceDescriptorHeap[descriptor];
			return texture.SampleLevel(g_anisotropicSampler, worldRayDirection, 0);
		}
		const Texture2D<float3> texture = ResourceDescriptorHeap[descriptor];
		return texture.SampleLevel(g_anisotropicSampler, Math::ToLatLongCoordinate(worldRayDirection), 0);
	}
	if (sceneData.EnvironmentLightColor.a >= 0)
	{
		return sceneData.EnvironmentLightColor.rgb;
	}
	return Color::FromSrgb(lerp(1, float3(0.5f, 0.7f, 1), (worldRayDirection.y + 1) * 0.5f));
}

void GetTextureCoordinates(
	VertexDesc vertexDesc,
	ByteAddressBuffer vertices,
	uint3 indices,
	float2 barycentrics,
	out float2 textureCoordinates[2]
)
{
	[[unroll]]
	for (uint i = 0; i < 2; i++)
	{
		if (vertexDesc.AttributeOffsets.TextureCoordinates[i] != ~0u)
		{
			float2 vertexTextureCoordinates[3];
			vertexDesc.LoadTextureCoordinates(vertices, indices, i, vertexTextureCoordinates);
			textureCoordinates[i] = Vertex::Interpolate(vertexTextureCoordinates, barycentrics);
		}
		else
		{
			textureCoordinates[i] = 0;
		}
	}
}

template <typename T>
T Sample(TextureMapInfo info, float2 textureCoordinates[2])
{
	const Texture2D<T> texture = ResourceDescriptorHeap[info.Descriptor];
	const float2 textureCoordinate = textureCoordinates[info.TextureCoordinateIndex];
	return texture.SampleLevel(g_anisotropicSampler, textureCoordinate, 0);
}

void EvaluateBaseColor(
	inout float4 baseColor,
	TextureMapInfo textureMapInfo, float2 textureCoordinates[2],
	inout bool hasSampledTexture
)
{
	if (any(baseColor > 0) && textureMapInfo.Descriptor != ~0u)
	{
		baseColor *= Sample<float4>(textureMapInfo, textureCoordinates);

		hasSampledTexture = true;
	}
}

void EvaluateTransmission(
	inout float transmission,
	TextureMapInfo textureMapInfo, float2 textureCoordinates[2],
	inout bool hasSampledTexture
)
{
	if (transmission > 0 && textureMapInfo.Descriptor != ~0u)
	{
		transmission *= Sample<float>(textureMapInfo, textureCoordinates);

		hasSampledTexture = true;
	}
}

void PerturbNormal(
	inout float3 N, float3 T,
	TextureMapInfo textureMapInfo, float2 textureCoordinates[2],
	inout bool hasSampledTexture
)
{
	if (textureMapInfo.Descriptor != ~0u)
	{
		const float3 normal = Geometry::UnpackLocalNormal(Sample<float2>(textureMapInfo, textureCoordinates));
		const float3x3 TBN = Math::CalculateTBN(N, T);
		N = normalize(Geometry::RotateVectorInverse(TBN, normal));

		hasSampledTexture = true;
	}
}

bool IsOpaque(Material material, TextureMapInfoArray textureMapInfoArray, float2 textureCoordinates[2])
{
	bool hasSampledTexture;
	EvaluateBaseColor(
		material.BaseColor,
		textureMapInfoArray[TextureMapType::BaseColor],
		textureCoordinates,
		hasSampledTexture
	);
	return material.BaseColor.a >= material.AlphaCutoff;
}

// Used for direct lighting
bool IsOpaque(Material material, TextureMapInfoArray textureMapInfoArray, float2 textureCoordinates[2], inout float3 visibility)
{
	bool hasSampledTexture;

	EvaluateBaseColor(
		material.BaseColor,
		textureMapInfoArray[TextureMapType::BaseColor], textureCoordinates,
		hasSampledTexture
	);

	if (material.AlphaMode != AlphaMode::Opaque)
	{
		const bool ret = material.BaseColor.a >= material.AlphaCutoff;
		visibility *= !ret;
		return ret;
	}

	if (material.Metallic > 0)
	{
		TextureMapInfo textureMapInfo;
		if ((textureMapInfo = textureMapInfoArray[TextureMapType::MetallicRoughness]).Descriptor != ~0u)
		{
			material.Metallic *= Sample<float3>(textureMapInfo, textureCoordinates).b;
		}
		else if ((textureMapInfo = textureMapInfoArray[TextureMapType::Metallic]).Descriptor != ~0u)
		{
			material.Metallic *= Sample<float>(textureMapInfo, textureCoordinates);
		}
		if (material.Metallic == 1)
		{
			visibility = 0;
			return true;
		}
	}

	EvaluateTransmission(
		material.Transmission,
		textureMapInfoArray[TextureMapType::Transmission], textureCoordinates,
		hasSampledTexture
	);
	return all((visibility *= (1 - material.Metallic) * material.BaseColor.rgb * material.Transmission) == 0);
}

Material EvaluateMaterial(
	inout float3 N, float3 T,
	Material material,
	TextureMapInfoArray textureMapInfoArray, float2 textureCoordinates[2],
	out bool hasSampledTexture
)
{
	hasSampledTexture = false;

	EvaluateBaseColor(
		material.BaseColor,
		textureMapInfoArray[TextureMapType::BaseColor], textureCoordinates,
		hasSampledTexture
	);

	TextureMapInfo textureMapInfo;

	if (any(material.GetEmission() > 0)
		&& (textureMapInfo = textureMapInfoArray[TextureMapType::EmissiveColor]).Descriptor != ~0u)
	{
		material.EmissiveColor *= Sample<float3>(textureMapInfo, textureCoordinates);

		hasSampledTexture = true;
	}

	if ((textureMapInfo = textureMapInfoArray[TextureMapType::MetallicRoughness]).Descriptor != ~0u)
	{
		if (material.Metallic > 0 || material.Roughness > 0)
		{
			const float3 value = Sample<float3>(textureMapInfo, textureCoordinates);
			material.Metallic *= value.b;
			material.Roughness *= value.g;

			hasSampledTexture = true;
		}
	}
	else
	{
		if (material.Metallic > 0
			&& (textureMapInfo = textureMapInfoArray[TextureMapType::Metallic]).Descriptor != ~0u)
		{
			material.Metallic *= Sample<float>(textureMapInfo, textureCoordinates);

			hasSampledTexture = true;
		}
		if (material.Roughness > 0
			&& (textureMapInfo = textureMapInfoArray[TextureMapType::Roughness]).Descriptor != ~0u)
		{
			material.Roughness *= Sample<float>(textureMapInfo, textureCoordinates);

			hasSampledTexture = true;
		}
	}

	if (material.Metallic < 1)
	{
		EvaluateTransmission(
			material.Transmission,
			textureMapInfoArray[TextureMapType::Transmission], textureCoordinates,
			hasSampledTexture
		);
	}

	if (any(T != 0))
	{
		PerturbNormal(
			N, T,
			textureMapInfoArray[TextureMapType::Normal],
			textureCoordinates,
			hasSampledTexture
		);
	}

	return material;
}
