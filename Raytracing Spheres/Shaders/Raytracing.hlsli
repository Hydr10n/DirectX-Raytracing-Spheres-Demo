#pragma once

#include "STL.hlsli"

#include "Math.hlsli"

#include "Camera.hlsli"

#include "Material.hlsli"

#include "HitInfo.hlsli"

#include "TriangleMeshHelpers.hlsli"

SamplerState g_anisotropicSampler : register(s0);

RaytracingAccelerationStructure g_scene : register(t0);

struct GlobalResourceDescriptorHeapIndices {
	uint
		InstanceResourceDescriptorHeapIndices,
		Camera,
		GlobalData, InstanceData,
		Motion,
		NormalRoughness,
		ViewZ,
		BaseColorMetalness,
		NoisyDiffuse, NoisySpecular,
		Output,
		EnvironmentLightCubeMap, EnvironmentCubeMap;
	uint3 _;
};
ConstantBuffer<GlobalResourceDescriptorHeapIndices> g_globalResourceDescriptorHeapIndices : register(b0);

struct InstanceResourceDescriptorHeapIndices {
	struct {
		uint Vertices, Indices;
		uint2 _;
	} TriangleMesh;
	struct {
		uint BaseColorMap, EmissiveColorMap, MetallicMap, RoughnessMap, AmbientOcclusionMap, OpacityMap, NormalMap;
		uint _;
	} Textures;
};
static const StructuredBuffer<InstanceResourceDescriptorHeapIndices> g_instanceResourceDescriptorHeapIndices = ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.InstanceResourceDescriptorHeapIndices];

static const Camera g_camera = (ConstantBuffer<Camera>)ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.Camera];

struct GlobalData {
	uint FrameIndex, MaxTraceRecursionDepth, SamplesPerPixel;
	bool IsRussianRouletteEnabled;
	float4 NRDHitDistanceParameters, EnvironmentLightColor;
	float4x4 EnvironmentLightCubeMapTransform;
	float4 EnvironmentColor;
	float4x4 EnvironmentCubeMapTransform;
};
static const GlobalData g_globalData = (ConstantBuffer<GlobalData>)ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.GlobalData];

struct InstanceData {
	float MaterialBaseColorAlphaThreshold;
	float3 _;
	Material Material;
	float4x4 TextureTransform, WorldToPreviousWorld;
};
static const StructuredBuffer<InstanceData> g_instanceData = ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.InstanceData];

static const TextureCube<float3> g_environmentLightCubeMap = ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.EnvironmentLightCubeMap];
static const TextureCube<float3> g_environmentCubeMap = ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.EnvironmentCubeMap];

static const RWTexture2D<float3> g_motion = ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.Motion];
static const RWTexture2D<float4> g_normalRoughness = ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.NormalRoughness];
static const RWTexture2D<float> g_viewZ = ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.ViewZ];
static const RWTexture2D<float4> g_baseColorMetalness = ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.BaseColorMetalness];
static const RWTexture2D<float4> g_noisyDiffuse = ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.NoisyDiffuse];
static const RWTexture2D<float4> g_noisySpecular = ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.NoisySpecular];
static const RWTexture2D<float4> g_output = ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.Output];

inline float3 GetEnvironmentLightColor(float3 worldRayDirection) {
	if (g_globalResourceDescriptorHeapIndices.EnvironmentLightCubeMap != ~0u) {
		return g_environmentLightCubeMap.SampleLevel(g_anisotropicSampler, normalize(STL::Geometry::RotateVector(g_globalData.EnvironmentLightCubeMapTransform, worldRayDirection)), 0);
	}
	if (g_globalData.EnvironmentLightColor.a >= 0) return g_globalData.EnvironmentLightColor.rgb;
	return lerp(1, float3(0.5f, 0.7f, 1), (worldRayDirection.y + 1) * 0.5f);
}

inline bool GetEnvironmentColor(float3 worldRayDirection, out float3 color) {
	bool ret;
	if ((ret = g_globalResourceDescriptorHeapIndices.EnvironmentCubeMap != ~0u)) {
		color = g_environmentCubeMap.SampleLevel(g_anisotropicSampler, normalize(STL::Geometry::RotateVector(g_globalData.EnvironmentCubeMapTransform, worldRayDirection)), 0);
	}
	else if ((ret = g_globalData.EnvironmentColor.a >= 0)) color = g_globalData.EnvironmentColor.rgb;
	return ret;
}

inline Material GetMaterial(uint instanceIndex, float2 textureCoordinate) {
	Material material;

	uint index;

	if ((index = g_instanceResourceDescriptorHeapIndices[instanceIndex].Textures.BaseColorMap) != ~0u) {
		const Texture2D<float4> texture = ResourceDescriptorHeap[index];
		material.BaseColor = texture.SampleLevel(g_anisotropicSampler, textureCoordinate, 0);
	}
	else material.BaseColor = g_instanceData[instanceIndex].Material.BaseColor;
	material.BaseColor.a = material.BaseColor.a > g_instanceData[instanceIndex].MaterialBaseColorAlphaThreshold ? 1 : saturate(material.BaseColor.a);

	if ((index = g_instanceResourceDescriptorHeapIndices[instanceIndex].Textures.EmissiveColorMap) != ~0u) {
		const Texture2D<float4> texture = ResourceDescriptorHeap[index];
		material.EmissiveColor = texture.SampleLevel(g_anisotropicSampler, textureCoordinate, 0);
	}
	else material.EmissiveColor = g_instanceData[instanceIndex].Material.EmissiveColor;

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
	hitInfo.Vertex.Normal = hitInfo.VertexUnmappedNormal = normalize(STL::Geometry::RotateVector((float3x3)objectToWorld, Vertex::Interpolate(normals, barycentrics)));
	HitInfo::SetFaceNormal(worldRayDirection, hitInfo.VertexUnmappedNormal);
	hitInfo.Vertex.TextureCoordinate = STL::Geometry::AffineTransform(g_instanceData[instanceIndex].TextureTransform, float3(Vertex::Interpolate(textureCoordinates, barycentrics), 0)).xy;
	if (instanceResourceDescriptorHeapIndices.Textures.NormalMap != ~0u) {
		const Texture2D<float3> normalMap = ResourceDescriptorHeap[instanceResourceDescriptorHeapIndices.Textures.NormalMap];
		const float3 positions[] = { vertices[indices[0]].Position, vertices[indices[1]].Position, vertices[indices[2]].Position };
		float3 T = Math::CalculateTangent(positions, textureCoordinates);
		const float3x3 TBN = float3x3(T, normalize(cross(hitInfo.Vertex.Normal, T)), hitInfo.Vertex.Normal);
		hitInfo.Vertex.Normal = normalize(STL::Geometry::RotateVectorInverse(TBN, normalMap.SampleLevel(g_anisotropicSampler, hitInfo.Vertex.TextureCoordinate, 0) * 2 - 1));
	}
	hitInfo.SetFaceNormal(worldRayDirection);
	return hitInfo;
}

struct RayCastResult {
	uint InstanceIndex;
	float HitDistance;
	HitInfo HitInfo;
	Material Material;
};

bool CastRay(RayDesc rayDesc, out RayCastResult rayCastResult) {
	RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
	q.TraceRayInline(g_scene, RAY_FLAG_NONE, ~0u, rayDesc);
	q.Proceed();
	const bool ret = q.CommittedStatus() != COMMITTED_NOTHING;
	if (ret) {
		rayCastResult.InstanceIndex = q.CommittedInstanceIndex();
		rayCastResult.HitDistance = q.CommittedRayT();
		rayCastResult.HitInfo = GetHitInfo(rayCastResult.InstanceIndex, rayDesc.Origin, rayDesc.Direction, rayCastResult.HitDistance, q.CommittedObjectToWorld3x4(), q.CommittedPrimitiveIndex(), q.CommittedTriangleBarycentrics());
		rayCastResult.Material = GetMaterial(rayCastResult.InstanceIndex, rayCastResult.HitInfo.Vertex.TextureCoordinate);
	}
	return ret;
}
