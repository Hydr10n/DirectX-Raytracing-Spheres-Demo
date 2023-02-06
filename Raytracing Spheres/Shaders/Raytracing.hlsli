#pragma once

#include "Camera.hlsli"

#include "Material.hlsli"

#include "HitInfo.hlsli"

#include "Math.hlsli"

#include "TriangleMeshHelpers.hlsli"

SamplerState g_anisotropicSampler : register(s0);

RaytracingAccelerationStructure g_scene : register(t0);

struct GlobalResourceDescriptorHeapIndices {
	uint
		InstanceResourceDescriptorHeapIndices,
		Camera,
		GlobalData, InstanceData,
		Output,
		EnvironmentLightCubeMap, EnvironmentCubeMap;
};
ConstantBuffer<GlobalResourceDescriptorHeapIndices> g_globalResourceDescriptorHeapIndices : register(b0);

struct InstanceResourceDescriptorHeapIndices {
	struct {
		uint Vertices, Indices;
		uint2 _;
	} TriangleMesh;
	struct { uint BaseColorMap, EmissiveColorMap, SpecularMap, MetallicMap, RoughnessMap, AmbientOcclusionMap, OpacityMap, NormalMap; } Textures;
};
static const StructuredBuffer<InstanceResourceDescriptorHeapIndices> g_instanceResourceDescriptorHeapIndices = ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.InstanceResourceDescriptorHeapIndices];

static const Camera g_camera = (ConstantBuffer<Camera>)ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.Camera];

struct GlobalData {
	uint FrameIndex, AccumulatedFrameIndex, RaytracingMaxTraceRecursionDepth, RaytracingSamplesPerPixel;
	float MaterialBaseColorAlphaThreshold;
	float3 _;
	float4 EnvironmentLightColor;
	float4x4 EnvironmentLightCubeMapTransform;
	float4 EnvironmentColor;
	float4x4 EnvironmentCubeMapTransform;
};
static const GlobalData g_globalData = (ConstantBuffer<GlobalData>)ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.GlobalData];

struct InstanceData {
	Material Material;
	float4x4 TextureTransform;
};
static const StructuredBuffer<InstanceData> g_instanceData = ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.InstanceData];

static const RWTexture2D<float4> g_output = ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.Output];

static const TextureCube<float4> g_environmentLightCubeMap = ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.EnvironmentLightCubeMap];
static const TextureCube<float4> g_environmentCubeMap = ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.EnvironmentCubeMap];

inline Material GetMaterial(uint instanceIndex, float2 textureCoordinate) {
	Material material;

	uint index;

	if ((index = g_instanceResourceDescriptorHeapIndices[instanceIndex].Textures.BaseColorMap) != ~0u) {
		const Texture2D<float4> texture = ResourceDescriptorHeap[index];
		material.BaseColor = texture.SampleLevel(g_anisotropicSampler, textureCoordinate, 0);
	}
	else material.BaseColor = g_instanceData[instanceIndex].Material.BaseColor;
	material.BaseColor.a = material.BaseColor.a > g_globalData.MaterialBaseColorAlphaThreshold ? 1 : saturate(material.BaseColor.a);

	if ((index = g_instanceResourceDescriptorHeapIndices[instanceIndex].Textures.EmissiveColorMap) != ~0u) {
		const Texture2D<float4> texture = ResourceDescriptorHeap[index];
		material.EmissiveColor = texture.SampleLevel(g_anisotropicSampler, textureCoordinate, 0);
	}
	else material.EmissiveColor = g_instanceData[instanceIndex].Material.EmissiveColor;

	if ((index = g_instanceResourceDescriptorHeapIndices[instanceIndex].Textures.SpecularMap) != ~0u) {
		const Texture2D<float3> texture = ResourceDescriptorHeap[index];
		material.Specular = texture.SampleLevel(g_anisotropicSampler, textureCoordinate, 0);
	}
	else material.Specular = g_instanceData[instanceIndex].Material.Specular;
	material.Specular = saturate(material.Specular);

	{
		/*
		 * glTF 2.0: Metallic (Red) | Roughness (Green)
		 * Others: Roughness (Green) | Metallic (Blue)
		 */

		const uint
			metallicMapIndex = g_instanceResourceDescriptorHeapIndices[instanceIndex].Textures.MetallicMap,
			roughnessMapIndex = g_instanceResourceDescriptorHeapIndices[instanceIndex].Textures.RoughnessMap,
			ambientOcclusionMapIndex = g_instanceResourceDescriptorHeapIndices[instanceIndex].Textures.AmbientOcclusionMap;
		const uint metallicMapChannel = metallicMapIndex == roughnessMapIndex && roughnessMapIndex == ambientOcclusionMapIndex ? 2 : 0;

		if (metallicMapIndex != ~0u) {
			const Texture2D<float3> texture = ResourceDescriptorHeap[metallicMapIndex];
			material.Metallic = texture.SampleLevel(g_anisotropicSampler, textureCoordinate, 0)[metallicMapChannel];
		}
		else material.Metallic = g_instanceData[instanceIndex].Material.Metallic;
		material.Metallic = saturate(material.Metallic);

		if (roughnessMapIndex != ~0u) {
			const Texture2D<float3> texture = ResourceDescriptorHeap[roughnessMapIndex];
			material.Roughness = texture.SampleLevel(g_anisotropicSampler, textureCoordinate, 0).g;
		}
		else material.Roughness = g_instanceData[instanceIndex].Material.Roughness;
		material.Roughness = clamp(material.Roughness, 1e-4f, 1);

		if (ambientOcclusionMapIndex != ~0u) {
			const Texture2D<float3> texture = ResourceDescriptorHeap[ambientOcclusionMapIndex];
			material.AmbientOcclusion = saturate(texture.SampleLevel(g_anisotropicSampler, textureCoordinate, 0)).r;
		}
		else material.AmbientOcclusion = 1;
	}

	if ((index = g_instanceResourceDescriptorHeapIndices[instanceIndex].Textures.OpacityMap) != ~0u) {
		const Texture2D<float> texture = ResourceDescriptorHeap[index];
		material.Opacity = texture.SampleLevel(g_anisotropicSampler, textureCoordinate, 0);
	}
	else material.Opacity = g_instanceData[instanceIndex].Material.Opacity;
	material.Opacity = saturate(material.Opacity);

	material.RefractiveIndex = max(1, g_instanceData[instanceIndex].Material.RefractiveIndex);

	return material;
}

inline HitInfo GetHitInfo(uint instanceIndex, float3 worldRayOrigin, float3 worldRayDirection, float rayT, float3x4 objectToWorld, uint primitiveIndex, float2 barycentrics) {
	const InstanceResourceDescriptorHeapIndices instanceResourceDescriptorHeapIndices = g_instanceResourceDescriptorHeapIndices[instanceIndex];

	const StructuredBuffer<VertexPositionNormalTexture> vertices = ResourceDescriptorHeap[instanceResourceDescriptorHeapIndices.TriangleMesh.Vertices];
	const uint3 indices = TriangleMeshHelpers::Load3Indices(ResourceDescriptorHeap[instanceResourceDescriptorHeapIndices.TriangleMesh.Indices], primitiveIndex);
	const float3 normals[] = { vertices[indices[0]].Normal, vertices[indices[1]].Normal, vertices[indices[2]].Normal };
	const float2 textureCoordinates[] = { vertices[indices[0]].TextureCoordinate, vertices[indices[1]].TextureCoordinate, vertices[indices[2]].TextureCoordinate };

	HitInfo hitInfo;
	hitInfo.Vertex.Position = worldRayOrigin + worldRayDirection * rayT;
	hitInfo.Vertex.Normal = hitInfo.VertexUnmappedNormal = normalize(mul((float3x3)objectToWorld, Vertex::Interpolate(normals, barycentrics)));
	HitInfo::SetFaceNormal(worldRayDirection, hitInfo.VertexUnmappedNormal);
	hitInfo.Vertex.TextureCoordinate = mul(g_instanceData[instanceIndex].TextureTransform, float4(Vertex::Interpolate(textureCoordinates, barycentrics), 0, 1)).xy;
	if (instanceResourceDescriptorHeapIndices.Textures.NormalMap != ~0u) {
		const Texture2D<float3> normalMap = ResourceDescriptorHeap[instanceResourceDescriptorHeapIndices.Textures.NormalMap];
		const float3
			positions[] = { vertices[indices[0]].Position, vertices[indices[1]].Position, vertices[indices[2]].Position },
			T = Math::CalculateTangent(positions, textureCoordinates);
		const float3x3 TBN = float3x3(T, normalize(cross(hitInfo.Vertex.Normal, T)), hitInfo.Vertex.Normal);
		hitInfo.Vertex.Normal = normalize(mul(normalMap.SampleLevel(g_anisotropicSampler, hitInfo.Vertex.TextureCoordinate, 0) * 2 - 1, TBN));
	}
	hitInfo.SetFaceNormal(worldRayDirection);
	return hitInfo;
}
