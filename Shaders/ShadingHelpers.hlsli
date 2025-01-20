#pragma once

#include "MeshHelpers.hlsli"

float3 GetEnvironmentLightColor(SceneData sceneData, float3 worldRayDirection)
{
	const SceneResourceDescriptorIndices indices = sceneData.ResourceDescriptorIndices;
	if (indices.EnvironmentLightTexture != ~0u)
	{
		worldRayDirection = normalize(Geometry::RotateVector((float3x3)sceneData.EnvironmentLightTextureTransform, worldRayDirection));
		if (sceneData.IsEnvironmentLightTextureCubeMap)
		{
			const TextureCube<float3> texture = ResourceDescriptorHeap[indices.EnvironmentLightTexture];
			return texture.SampleLevel(g_anisotropicSampler, worldRayDirection, 0);
		}
		const Texture2D<float3> texture = ResourceDescriptorHeap[indices.EnvironmentLightTexture];
		return texture.SampleLevel(g_anisotropicSampler, Math::ToLatLongCoordinate(worldRayDirection), 0);
	}
	if (sceneData.EnvironmentLightColor.a >= 0)
	{
		return sceneData.EnvironmentLightColor.rgb;
	}
	return Color::FromSrgb(lerp(1, float3(0.5f, 0.7f, 1), (worldRayDirection.y + 1) * 0.5f));
}

float2 GetTextureCoordinate(ObjectData objectData, uint primitiveIndex, float2 barycentrics)
{
	const MeshResourceDescriptorIndices meshIndices = objectData.ResourceDescriptorIndices.Mesh;
	const uint3 indices = MeshHelpers::Load3Indices(ResourceDescriptorHeap[meshIndices.Indices], primitiveIndex);
	float2 textureCoordinates[3];
	objectData.VertexDesc.LoadTextureCoordinates(ResourceDescriptorHeap[meshIndices.Vertices], indices, textureCoordinates);
	return Vertex::Interpolate(textureCoordinates, barycentrics);
}

float GetTransmission(ObjectData objectData, float2 textureCoordinate, float baseColorAlpha = -1)
{
	const TextureMapResourceDescriptorIndices indices = objectData.ResourceDescriptorIndices.TextureMaps;
	uint index;
	float transmission = objectData.Material.Transmission;
	if ((index = indices.Transmission) != ~0u)
	{
		if (transmission > 0)
		{
			const Texture2D<float> texture = ResourceDescriptorHeap[index];
			transmission *= texture.SampleLevel(g_anisotropicSampler, textureCoordinate, 0);
		}
		return transmission;
	}
	if (transmission > 0)
	{
		return transmission;
	}
	if ((index = indices.Opacity) != ~0u)
	{
		const Texture2D<float> texture = ResourceDescriptorHeap[index];
		return 1 - texture.SampleLevel(g_anisotropicSampler, textureCoordinate, 0);
	}
	transmission = 1 - baseColorAlpha;
	if (baseColorAlpha < 0)
	{
		baseColorAlpha = objectData.Material.BaseColor.a;
		if (baseColorAlpha > 0 && (index = indices.BaseColor) != ~0u)
		{
			const Texture2D<float4> texture = ResourceDescriptorHeap[index];
			transmission *= 1 - texture.SampleLevel(g_anisotropicSampler, textureCoordinate, 0).a;
		}
	}
	return transmission;
}

bool IsOpaque(ObjectData objectData, float2 textureCoordinate)
{
	return 1 - GetTransmission(objectData, textureCoordinate) >= objectData.Material.AlphaCutoff;
}

// Used for direct lighting
bool IsOpaque(ObjectData objectData, float2 textureCoordinate, inout float3 visibility)
{
	Material material = objectData.Material;

	if (material.IOR != BRDF::IOR::Vacuum)
	{
		visibility = 0;
		return true;
	}

	const TextureMapResourceDescriptorIndices indices = objectData.ResourceDescriptorIndices.TextureMaps;

	uint index;

	if (material.Metallic > 0)
	{
		if ((index = indices.Metallic) != ~0u)
		{
			const Texture2D<float3> texture = ResourceDescriptorHeap[index];
			material.Metallic *= texture.SampleLevel(g_anisotropicSampler, textureCoordinate, 0).b;
		}
		if (material.Metallic == 1)
		{
			visibility = 0;
			return true;
		}
	}

	if (any(material.BaseColor > 0) && (index = indices.BaseColor) != ~0u)
	{
		const Texture2D<float4> texture = ResourceDescriptorHeap[index];
		material.BaseColor *= texture.SampleLevel(g_anisotropicSampler, textureCoordinate, 0);
	}

	material.Transmission = GetTransmission(objectData, textureCoordinate, material.BaseColor.a);

	if (material.AlphaMode != AlphaMode::Opaque)
	{
		const bool ret = 1 - material.Transmission >= material.AlphaCutoff;
		visibility *= !ret;
		return ret;
	}

	return all((visibility *= (1 - material.Metallic) * material.BaseColor.rgb * material.Transmission) == 0);
}

Material GetMaterial(ObjectData objectData, float2 textureCoordinate)
{
	Material material = objectData.Material;

	const TextureMapResourceDescriptorIndices indices = objectData.ResourceDescriptorIndices.TextureMaps;

	uint index;

	if (any(material.BaseColor > 0) && (index = indices.BaseColor) != ~0u)
	{
		const Texture2D<float4> texture = ResourceDescriptorHeap[index];
		material.BaseColor *= texture.SampleLevel(g_anisotropicSampler, textureCoordinate, 0);
	}

	material.Transmission = GetTransmission(objectData, textureCoordinate, material.BaseColor.a);

	if (any(material.GetEmission() > 0) && (index = indices.EmissiveColor) != ~0u)
	{
		const Texture2D<float3> texture = ResourceDescriptorHeap[index];
		material.EmissiveColor *= texture.SampleLevel(g_anisotropicSampler, textureCoordinate, 0);
	}

	if (material.Metallic > 0 && (index = indices.Metallic) != ~0u)
	{
		const Texture2D<float3> texture = ResourceDescriptorHeap[index];
		material.Metallic *= texture.SampleLevel(g_anisotropicSampler, textureCoordinate, 0).b;
	}

	if (material.Roughness > 0 && (index = indices.Roughness) != ~0u)
	{
		const Texture2D<float3> texture = ResourceDescriptorHeap[index];
		material.Roughness *= texture.SampleLevel(g_anisotropicSampler, textureCoordinate, 0).g;
	}

	return material;
}
