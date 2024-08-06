#pragma once

#include "MeshHelpers.hlsli"

float2 GetTextureCoordinate(uint objectIndex, uint primitiveIndex, float2 barycentrics)
{
	const ObjectResourceDescriptorIndices resourceDescriptorIndices = g_objectData[objectIndex].ResourceDescriptorIndices;
	const uint3 indices = MeshHelpers::Load3Indices(ResourceDescriptorHeap[resourceDescriptorIndices.Mesh.Indices], primitiveIndex);
	float2 textureCoordinates[3];
	g_objectData[objectIndex].VertexDesc.LoadTextureCoordinates(ResourceDescriptorHeap[resourceDescriptorIndices.Mesh.Vertices], indices, textureCoordinates);
	return Vertex::Interpolate(textureCoordinates, barycentrics);
}

float GetTransmission(uint objectIndex, float2 textureCoordinate, float baseColorAlpha = -1)
{
	const ObjectResourceDescriptorIndices resourceDescriptorIndices = g_objectData[objectIndex].ResourceDescriptorIndices;
	uint index;
	if ((index = resourceDescriptorIndices.TextureMaps.Transmission) != ~0u)
	{
		const Texture2D<float> texture = ResourceDescriptorHeap[index];
		return texture.SampleLevel(g_anisotropicSampler, textureCoordinate, 0);
	}
	const float transmission = g_objectData[objectIndex].Material.Transmission;
	if (transmission > 0)
	{
		return transmission;
	}
	if ((index = resourceDescriptorIndices.TextureMaps.Opacity) != ~0u)
	{
		const Texture2D<float> texture = ResourceDescriptorHeap[index];
		return 1 - texture.SampleLevel(g_anisotropicSampler, textureCoordinate, 0);
	}
	if (baseColorAlpha < 0)
	{
		if ((index = resourceDescriptorIndices.TextureMaps.BaseColor) != ~0u)
		{
			const Texture2D<float4> texture = ResourceDescriptorHeap[index];
			return 1 - texture.SampleLevel(g_anisotropicSampler, textureCoordinate, 0).a;
		}
		return 1 - g_objectData[objectIndex].Material.BaseColor.a;
	}
	return 1 - baseColorAlpha;
}

bool IsOpaque(uint objectIndex, float2 textureCoordinate)
{
	/*TODO: Alpha Blending*/

	const AlphaMode alphaMode = g_objectData[objectIndex].Material.AlphaMode;
	if (alphaMode == AlphaMode::Opaque)
	{
		return true;
	}

	const float opacity = 1 - GetTransmission(objectIndex, textureCoordinate);
	if (opacity >= g_objectData[objectIndex].Material.AlphaThreshold)
	{
		return true;
	}

	return false;
}

Material GetMaterial(uint objectIndex, float2 textureCoordinate)
{
	const ObjectResourceDescriptorIndices resourceDescriptorIndices = g_objectData[objectIndex].ResourceDescriptorIndices;

	Material material = g_objectData[objectIndex].Material;

	uint index;

	if ((index = resourceDescriptorIndices.TextureMaps.BaseColor) != ~0u)
	{
		const Texture2D<float4> texture = ResourceDescriptorHeap[index];
		material.BaseColor = texture.SampleLevel(g_anisotropicSampler, textureCoordinate, 0);
	}

	if ((index = resourceDescriptorIndices.TextureMaps.EmissiveColor) != ~0u)
	{
		const Texture2D<float3> texture = ResourceDescriptorHeap[index];
		material.EmissiveColor = texture.SampleLevel(g_anisotropicSampler, textureCoordinate, 0);
	}

	{
		/*
		 * glTF 2.0: Metallic (Red) | Roughness (Green)
		 * Others: Roughness (Green) | Metallic (Blue)
		 */

		const uint
			metallicMapIndex = resourceDescriptorIndices.TextureMaps.Metallic,
			roughnessMapIndex = resourceDescriptorIndices.TextureMaps.Roughness,
			ambientOcclusionMapIndex = resourceDescriptorIndices.TextureMaps.AmbientOcclusion;
		const uint metallicMapChannel = metallicMapIndex == roughnessMapIndex && roughnessMapIndex == ambientOcclusionMapIndex ? 2 : 0;

		if (metallicMapIndex != ~0u)
		{
			const Texture2D<float3> texture = ResourceDescriptorHeap[metallicMapIndex];
			material.Metallic = texture.SampleLevel(g_anisotropicSampler, textureCoordinate, 0)[metallicMapChannel];
		}

		if (roughnessMapIndex != ~0u)
		{
			const Texture2D<float3> texture = ResourceDescriptorHeap[roughnessMapIndex];
			material.Roughness = texture.SampleLevel(g_anisotropicSampler, textureCoordinate, 0).g;
		}
	}

	material.Transmission = GetTransmission(objectIndex, textureCoordinate, material.BaseColor.a);

	return material;
}
