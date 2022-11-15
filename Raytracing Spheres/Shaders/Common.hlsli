#pragma once

#include "Camera.hlsli"

#include "Material.hlsli"

#include "HitInfo.hlsli"

#include "Math.hlsli"

#include "TriangleMeshHelpers.hlsli"

SamplerState g_anisotropicWrap : register(s0);

RaytracingAccelerationStructure g_scene : register(t0);

struct GlobalResourceDescriptorHeapIndices {
	uint
		LocalResourceDescriptorHeapIndices,
		Camera,
		GlobalData, LocalData,
		Output,
		EnvironmentLightCubeMap, EnvironmentCubeMap;
	uint _;
};
ConstantBuffer<GlobalResourceDescriptorHeapIndices> g_globalResourceDescriptorHeapIndices : register(b0);

struct LocalResourceDescriptorHeapIndices {
	struct {
		uint Vertices, Indices;
		uint2 _;
	} TriangleMesh;
	struct { uint BaseColorMap, EmissiveMap, SpecularMap, MetallicMap, RoughnessMap, AmbientOcclusionMap, OpacityMap, NormalMap; } Textures;
};
static const StructuredBuffer<LocalResourceDescriptorHeapIndices> g_localResourceDescriptorHeapIndices = ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.LocalResourceDescriptorHeapIndices];

static const Camera g_camera = (ConstantBuffer<Camera>)ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.Camera];

struct GlobalData {
	uint RaytracingMaxTraceRecursionDepth, RaytracingSamplesPerPixel, FrameCount, AccumulatedFrameIndex;
	float4 EnvironmentLightColor;
	float4x4 EnvironmentLightCubeMapTransform;
	float4 EnvironmentColor;
	float4x4 EnvironmentCubeMapTransform;
};
static const GlobalData g_globalData = (ConstantBuffer<GlobalData>)ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.GlobalData];

struct LocalData {
	Material Material;
	float4x4 TextureTransform;
};
static const StructuredBuffer<LocalData> g_localData = ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.LocalData];

static const RWTexture2D<float4> g_output = ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.Output];

static const TextureCube<float4> g_environmentLightCubeMap = ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.EnvironmentLightCubeMap];
static const TextureCube<float4> g_environmentCubeMap = ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.EnvironmentCubeMap];

inline Material GetMaterial(uint instanceIndex, float2 textureCoordinate) {
	Material material;

	uint index;

	if ((index = g_localResourceDescriptorHeapIndices[instanceIndex].Textures.BaseColorMap) != ~0u) {
		const Texture2D<float4> texture = ResourceDescriptorHeap[index];
		material.BaseColor = texture.SampleLevel(g_anisotropicWrap, textureCoordinate, 0);
	}
	else material.BaseColor = g_localData[instanceIndex].Material.BaseColor;

	if ((index = g_localResourceDescriptorHeapIndices[instanceIndex].Textures.EmissiveMap) != ~0u) {
		const Texture2D<float4> texture = ResourceDescriptorHeap[index];
		material.EmissiveColor = texture.SampleLevel(g_anisotropicWrap, textureCoordinate, 0);
	}
	else material.EmissiveColor = g_localData[instanceIndex].Material.EmissiveColor;

	if ((index = g_localResourceDescriptorHeapIndices[instanceIndex].Textures.SpecularMap) != ~0u) {
		const Texture2D<float3> texture = ResourceDescriptorHeap[index];
		material.Specular = texture.SampleLevel(g_anisotropicWrap, textureCoordinate, 0);
	}
	else material.Specular = g_localData[instanceIndex].Material.Specular;

	{
		/*
		 * glTF 2.0: Metallic (Red) | Roughness (Green)
		 * Others: Roughness (Green) | Metallic (Blue)
		 */

		const uint
			metallicMapIndex = g_localResourceDescriptorHeapIndices[instanceIndex].Textures.MetallicMap,
			roughnessMapIndex = g_localResourceDescriptorHeapIndices[instanceIndex].Textures.RoughnessMap,
			ambientOcclusionMapIndex = g_localResourceDescriptorHeapIndices[instanceIndex].Textures.AmbientOcclusionMap;
		const uint metallicMapChannel = metallicMapIndex == roughnessMapIndex && roughnessMapIndex == ambientOcclusionMapIndex ? 2 : 0;

		if (metallicMapIndex != ~0u) {
			const Texture2D<float3> texture = ResourceDescriptorHeap[metallicMapIndex];
			material.Metallic = texture.SampleLevel(g_anisotropicWrap, textureCoordinate, 0)[metallicMapChannel];
		}
		else material.Metallic = g_localData[instanceIndex].Material.Metallic;
		material.Metallic = saturate(material.Metallic);

		if (roughnessMapIndex != ~0u) {
			const Texture2D<float3> texture = ResourceDescriptorHeap[roughnessMapIndex];
			material.Roughness = texture.SampleLevel(g_anisotropicWrap, textureCoordinate, 0).g;
		}
		else material.Roughness = g_localData[instanceIndex].Material.Roughness;
		material.Roughness = clamp(material.Roughness, 1e-4f, 1);

		if (ambientOcclusionMapIndex != ~0u) {
			const Texture2D<float3> texture = ResourceDescriptorHeap[ambientOcclusionMapIndex];
			material.AmbientOcclusion = saturate(texture.SampleLevel(g_anisotropicWrap, textureCoordinate, 0)).r;
		}
		else material.AmbientOcclusion = 1;
	}

	if ((index = g_localResourceDescriptorHeapIndices[instanceIndex].Textures.OpacityMap) != ~0u) {
		const Texture2D<float> texture = ResourceDescriptorHeap[index];
		material.Opacity = texture.SampleLevel(g_anisotropicWrap, textureCoordinate, 0);
	}
	else material.Opacity = g_localData[instanceIndex].Material.Opacity;
	material.Opacity = saturate(material.Opacity);

	material.RefractiveIndex = g_localData[instanceIndex].Material.RefractiveIndex;

	return material;
}

inline HitInfo GetHitInfo(uint instanceIndex, float3 worldRayOrigin, float3 worldRayDirection, float rayT, float4x3 objectToWorld, uint primitiveIndex, float2 barycentrics) {
	const LocalResourceDescriptorHeapIndices localResourceDescriptorHeapIndices = g_localResourceDescriptorHeapIndices[instanceIndex];

	const StructuredBuffer<VertexPositionNormalTexture> vertices = ResourceDescriptorHeap[localResourceDescriptorHeapIndices.TriangleMesh.Vertices];
	const uint3 indices = TriangleMeshHelpers::Load3Indices(ResourceDescriptorHeap[localResourceDescriptorHeapIndices.TriangleMesh.Indices], primitiveIndex);
	const float3 normals[] = { vertices[indices[0]].Normal, vertices[indices[1]].Normal, vertices[indices[2]].Normal };
	const float2 textureCoordinates[] = { vertices[indices[0]].TextureCoordinate, vertices[indices[1]].TextureCoordinate, vertices[indices[2]].TextureCoordinate };

	HitInfo hitInfo;
	hitInfo.Vertex.Position = worldRayOrigin + worldRayDirection * rayT;
	hitInfo.Vertex.Normal = hitInfo.VertexUnmappedNormal = normalize(mul(normalize(Vertex::Interpolate(normals, barycentrics)), (float3x3)objectToWorld));
	HitInfo::SetFaceNormal(worldRayDirection, hitInfo.VertexUnmappedNormal);
	hitInfo.Vertex.TextureCoordinate = mul(float4(Vertex::Interpolate(textureCoordinates, barycentrics), 0, 1), g_localData[instanceIndex].TextureTransform).xy;
	if (localResourceDescriptorHeapIndices.Textures.NormalMap != ~0u) {
		const Texture2D<float3> normalMap = ResourceDescriptorHeap[localResourceDescriptorHeapIndices.Textures.NormalMap];
		const float3 positions[] = { vertices[indices[0]].Position, vertices[indices[1]].Position, vertices[indices[2]].Position };
		const float3 T = Math::CalculateTangent(positions, textureCoordinates);
		const float3x3 TBN = float3x3(T, normalize(cross(hitInfo.Vertex.Normal, T)), hitInfo.Vertex.Normal);
		hitInfo.Vertex.Normal = normalize(mul(normalize(normalMap.SampleLevel(g_anisotropicWrap, hitInfo.Vertex.TextureCoordinate, 0) * 2 - 1), TBN));
	}
	hitInfo.SetFaceNormal(worldRayDirection);
	return hitInfo;
}
