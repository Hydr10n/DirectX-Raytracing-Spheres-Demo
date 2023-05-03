#pragma once

#include "STL.hlsli"

#include "Math.hlsli"

#include "Camera.hlsli"

#include "Material.hlsli"

#include "HitInfo.hlsli"

#include "RaytracingHelpers.hlsli"

#include "TriangleMeshHelpers.hlsli"

SamplerState g_anisotropicSampler : register(s0);

RaytracingAccelerationStructure g_scene : register(t0);

struct GlobalResourceDescriptorHeapIndices {
	uint
		Camera,
		GlobalData,
		InstanceData,
		ObjectResourceDescriptorHeapIndices, ObjectData,
		Motion,
		NormalRoughness,
		ViewZ,
		BaseColorMetalness,
		NoisyDiffuse, NoisySpecular,
		Output,
		EnvironmentLightCubeMap, EnvironmentCubeMap;
	uint2 _;
};
ConstantBuffer<GlobalResourceDescriptorHeapIndices> g_globalResourceDescriptorHeapIndices : register(b0);

static const Camera g_camera = (ConstantBuffer<Camera>)ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.Camera];

struct GlobalData {
	uint FrameIndex, MaxTraceRecursionDepth, SamplesPerPixel;
	bool IsRussianRouletteEnabled, IsWorldStatic;
	uint3 _;
	float4 NRDHitDistanceParameters, EnvironmentLightColor;
	float4x4 EnvironmentLightCubeMapTransform;
	float4 EnvironmentColor;
	float4x4 EnvironmentCubeMapTransform;
};
static const GlobalData g_globalData = (ConstantBuffer<GlobalData>)ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.GlobalData];

struct InstanceData {
	bool IsStatic;
	uint3 _;
	float4x4 PreviousObjectToWorld;
};
static const StructuredBuffer<InstanceData> g_instanceData = ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.InstanceData];

struct ObjectResourceDescriptorHeapIndices {
	struct {
		uint Vertices, Indices;
		uint2 _;
	} TriangleMesh;
	struct {
		uint BaseColorMap, EmissiveColorMap, MetallicMap, RoughnessMap, AmbientOcclusionMap, TransmissionMap, OpacityMap, NormalMap;
	} Textures;
};
static const StructuredBuffer<ObjectResourceDescriptorHeapIndices> g_objectResourceDescriptorHeapIndices = ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.ObjectResourceDescriptorHeapIndices];

struct ObjectData {
	Material Material;
	float4x4 TextureTransform;
};
static const StructuredBuffer<ObjectData> g_objectData = ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.ObjectData];

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

inline float2 GetTextureCoordinate(uint objectIndex, uint primitiveIndex, float2 barycentrics) {
	const ObjectResourceDescriptorHeapIndices objectResourceDescriptorHeapIndices = g_objectResourceDescriptorHeapIndices[objectIndex];

	const StructuredBuffer<VertexPositionNormalTextureTangent> vertices = ResourceDescriptorHeap[objectResourceDescriptorHeapIndices.TriangleMesh.Vertices];
	const uint3 indices = TriangleMeshHelpers::Load3Indices(ResourceDescriptorHeap[objectResourceDescriptorHeapIndices.TriangleMesh.Indices], primitiveIndex);
	const float2 textureCoordinates[] = { vertices[indices[0]].TextureCoordinate, vertices[indices[1]].TextureCoordinate, vertices[indices[2]].TextureCoordinate };
	return Vertex::Interpolate(textureCoordinates, barycentrics);
}

inline float GetOpacity(uint objectIndex, float2 textureCoordinate, bool getBaseColorAlpha = true) {
	uint index;
	if ((index = g_objectResourceDescriptorHeapIndices[objectIndex].Textures.TransmissionMap) != ~0u) {
		const Texture2D<float> texture = ResourceDescriptorHeap[index];
		return 1 - texture.SampleLevel(g_anisotropicSampler, textureCoordinate, 0);
	}
	if ((index = g_objectResourceDescriptorHeapIndices[objectIndex].Textures.OpacityMap) != ~0u) {
		const Texture2D<float> texture = ResourceDescriptorHeap[index];
		return texture.SampleLevel(g_anisotropicSampler, textureCoordinate, 0);
	}
	if (g_objectData[objectIndex].Material.AlphaMode != AlphaMode::Opaque && getBaseColorAlpha) {
		if ((index = g_objectResourceDescriptorHeapIndices[objectIndex].Textures.BaseColorMap) != ~0u) {
			const Texture2D<float4> texture = ResourceDescriptorHeap[index];
			return texture.SampleLevel(g_anisotropicSampler, textureCoordinate, 0).a;
		}
		return min(g_objectData[objectIndex].Material.Opacity, g_objectData[objectIndex].Material.BaseColor.a);
	}
	return g_objectData[objectIndex].Material.Opacity;
}

inline Material GetMaterial(uint objectIndex, float2 textureCoordinate) {
	Material material;

	uint index;

	if ((index = g_objectResourceDescriptorHeapIndices[objectIndex].Textures.BaseColorMap) != ~0u) {
		const Texture2D<float4> texture = ResourceDescriptorHeap[index];
		material.BaseColor = texture.SampleLevel(g_anisotropicSampler, textureCoordinate, 0);
	}
	else material.BaseColor = g_objectData[objectIndex].Material.BaseColor;

	if ((index = g_objectResourceDescriptorHeapIndices[objectIndex].Textures.EmissiveColorMap) != ~0u) {
		const Texture2D<float4> texture = ResourceDescriptorHeap[index];
		material.EmissiveColor = texture.SampleLevel(g_anisotropicSampler, textureCoordinate, 0);
	}
	else material.EmissiveColor = g_objectData[objectIndex].Material.EmissiveColor;

	{
		/*
		 * glTF 2.0: Metallic (Red) | Roughness (Green)
		 * Others: Roughness (Green) | Metallic (Blue)
		 */

		const uint
			metallicMapIndex = g_objectResourceDescriptorHeapIndices[objectIndex].Textures.MetallicMap,
			roughnessMapIndex = g_objectResourceDescriptorHeapIndices[objectIndex].Textures.RoughnessMap,
			ambientOcclusionMapIndex = g_objectResourceDescriptorHeapIndices[objectIndex].Textures.AmbientOcclusionMap;
		const uint metallicMapChannel = metallicMapIndex == roughnessMapIndex && roughnessMapIndex == ambientOcclusionMapIndex ? 2 : 0;

		if (metallicMapIndex != ~0u) {
			const Texture2D<float3> texture = ResourceDescriptorHeap[metallicMapIndex];
			material.Metallic = texture.SampleLevel(g_anisotropicSampler, textureCoordinate, 0)[metallicMapChannel];
		}
		else material.Metallic = g_objectData[objectIndex].Material.Metallic;

		if (roughnessMapIndex != ~0u) {
			const Texture2D<float3> texture = ResourceDescriptorHeap[roughnessMapIndex];
			material.Roughness = texture.SampleLevel(g_anisotropicSampler, textureCoordinate, 0).g;
		}
		else material.Roughness = g_objectData[objectIndex].Material.Roughness;
		material.Roughness = max(material.Roughness, 1e-4f);

		if (ambientOcclusionMapIndex != ~0u) {
			const Texture2D<float3> texture = ResourceDescriptorHeap[ambientOcclusionMapIndex];
			material.AmbientOcclusion = texture.SampleLevel(g_anisotropicSampler, textureCoordinate, 0).r;
		}
		else material.AmbientOcclusion = 1;
	}

	material.Opacity = GetOpacity(objectIndex, textureCoordinate, false);

	material.RefractiveIndex = max(1, g_objectData[objectIndex].Material.RefractiveIndex);

	return material;
}

inline HitInfo GetHitInfo(uint objectIndex, uint primitiveIndex, float2 barycentrics, float3x4 objectToWorld, bool isFrontFace, float3 worldRayDirection) {
	const ObjectResourceDescriptorHeapIndices objectResourceDescriptorHeapIndices = g_objectResourceDescriptorHeapIndices[objectIndex];

	const StructuredBuffer<VertexPositionNormalTexture> vertices = ResourceDescriptorHeap[objectResourceDescriptorHeapIndices.TriangleMesh.Vertices];
	const uint3 indices = TriangleMeshHelpers::Load3Indices(ResourceDescriptorHeap[objectResourceDescriptorHeapIndices.TriangleMesh.Indices], primitiveIndex);
	const float3
		positions[] = { vertices[indices[0]].Position, vertices[indices[1]].Position, vertices[indices[2]].Position },
		normals[] = { vertices[indices[0]].Normal, vertices[indices[1]].Normal, vertices[indices[2]].Normal };
	const float2 textureCoordinates[] = { vertices[indices[0]].TextureCoordinate, vertices[indices[1]].TextureCoordinate, vertices[indices[2]].TextureCoordinate };
	const float3 position = Vertex::Interpolate(positions, barycentrics), normal = normalize(Vertex::Interpolate(normals, barycentrics));

	HitInfo hitInfo;
	hitInfo.Vertex.Position = STL::Geometry::AffineTransform(objectToWorld, RaytracingHelpers::OffsetRay(position, isFrontFace ? normal : -normal));
	hitInfo.ObjectVertexPosition = position;
	hitInfo.Vertex.Normal = hitInfo.UnmappedVertexNormal = normalize(STL::Geometry::RotateVector((float3x3)objectToWorld, normal));
	HitInfo::SetFaceNormal(worldRayDirection, hitInfo.UnmappedVertexNormal);
	hitInfo.Vertex.TextureCoordinate = STL::Geometry::AffineTransform(g_objectData[objectIndex].TextureTransform, float3(Vertex::Interpolate(textureCoordinates, barycentrics), 0)).xy;
	if (objectResourceDescriptorHeapIndices.Textures.NormalMap != ~0u) {
		const Texture2D<float3> texture = ResourceDescriptorHeap[objectResourceDescriptorHeapIndices.Textures.NormalMap];
		const float3 T = Math::CalculateTangent(positions, textureCoordinates);
		const float3x3 TBN = float3x3(T, normalize(cross(hitInfo.Vertex.Normal, T)), hitInfo.Vertex.Normal);
		hitInfo.Vertex.Normal = normalize(STL::Geometry::RotateVectorInverse(TBN, texture.SampleLevel(g_anisotropicSampler, hitInfo.Vertex.TextureCoordinate, 0) * 2 - 1));
	}
	hitInfo.SetFaceNormal(worldRayDirection);
	return hitInfo;
}

struct RayCastResult {
	uint InstanceIndex, ObjectIndex;
	float HitDistance;
	HitInfo HitInfo;
	Material Material;
};

bool CastRay(RayDesc rayDesc, out RayCastResult rayCastResult) {
	RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
	q.TraceRayInline(g_scene, RAY_FLAG_NONE, ~0u, rayDesc);

	while (q.Proceed()) {
		//TODO: Alpha Blending
		const uint objectIndex = q.CandidateGeometryIndex() + q.CandidateInstanceID();
		const float2 textureCoordinate = GetTextureCoordinate(objectIndex, q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics());
		if (g_objectData[objectIndex].Material.AlphaMode == AlphaMode::Opaque || GetOpacity(objectIndex, textureCoordinate) >= g_objectData[objectIndex].Material.AlphaThreshold) {
			q.CommitNonOpaqueTriangleHit();
		}
	}

	const bool ret = q.CommittedStatus() != COMMITTED_NOTHING;
	if (ret) {
		rayCastResult.InstanceIndex = q.CommittedInstanceIndex();
		rayCastResult.ObjectIndex = q.CommittedGeometryIndex() + q.CommittedInstanceID();
		rayCastResult.HitDistance = q.CommittedRayT();
		rayCastResult.HitInfo = GetHitInfo(rayCastResult.ObjectIndex, q.CommittedPrimitiveIndex(), q.CommittedTriangleBarycentrics(), q.CommittedObjectToWorld3x4(), q.CommittedTriangleFrontFace(), rayDesc.Direction);
		rayCastResult.Material = GetMaterial(rayCastResult.ObjectIndex, rayCastResult.HitInfo.Vertex.TextureCoordinate);
	}
	return ret;
}
