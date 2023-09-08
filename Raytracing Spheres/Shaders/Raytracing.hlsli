#pragma once

#include "STL.hlsli"

#include "Math.hlsli"

#include "Camera.hlsli"

#include "Material.hlsli"

#include "HitInfo.hlsli"

#include "MeshHelpers.hlsli"

SamplerState g_anisotropicSampler : register(s0);

RaytracingAccelerationStructure g_scene : register(t0);

cbuffer _ : register(b0) { uint2 g_renderSize; }

struct GlobalResourceDescriptorHeapIndices {
	uint
		InGraphicsSettings,
		InCamera,
		InSceneData,
		InInstanceData,
		InObjectResourceDescriptorHeapIndices, InObjectData,
		InEnvironmentLightCubeMap, InEnvironmentCubeMap,
		Output,
		OutDepth,
		OutMotionVectors,
		OutBaseColorMetalness,
		OutEmissiveColor,
		OutNormalRoughness,
		OutNoisyDiffuse, OutNoisySpecular;
};
ConstantBuffer<GlobalResourceDescriptorHeapIndices> g_globalResourceDescriptorHeapIndices : register(b1);

struct GraphicsSettings {
	uint FrameIndex, MaxNumberOfBounces, SamplesPerPixel;
	bool IsRussianRouletteEnabled;
	float4 NRDHitDistanceParameters;
};
static const GraphicsSettings g_graphicsSettings = (ConstantBuffer<GraphicsSettings>)ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.InGraphicsSettings];

static const Camera g_camera = (ConstantBuffer<Camera>)ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.InCamera];

struct SceneData {
	bool IsStatic;
	uint3 _;
	float4 EnvironmentLightColor;
	float4x4 EnvironmentLightCubeMapTransform;
	float4 EnvironmentColor;
	float4x4 EnvironmentCubeMapTransform;
};
static const SceneData g_sceneData = (ConstantBuffer<SceneData>)ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.InSceneData];

struct InstanceData {
	bool IsStatic;
	float4x4 PreviousObjectToWorld;
};
static const StructuredBuffer<InstanceData> g_instanceData = ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.InInstanceData];

struct ObjectResourceDescriptorHeapIndices {
	struct { uint Vertices, Indices, MotionVectors; } Mesh;
	struct {
		uint BaseColorMap, EmissiveColorMap, MetallicMap, RoughnessMap, AmbientOcclusionMap, TransmissionMap, OpacityMap, NormalMap;
	} Textures;
};
static const StructuredBuffer<ObjectResourceDescriptorHeapIndices> g_objectResourceDescriptorHeapIndices = ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.InObjectResourceDescriptorHeapIndices];

struct ObjectData {
	Material Material;
	float4x4 TextureTransform;
};
static const StructuredBuffer<ObjectData> g_objectData = ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.InObjectData];

static const TextureCube<float3> g_environmentLightCubeMap = ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.InEnvironmentLightCubeMap];
static const TextureCube<float3> g_environmentCubeMap = ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.InEnvironmentCubeMap];

static const RWTexture2D<float4> g_output = ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.Output];
static const RWTexture2D<float> g_depth = ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.OutDepth];
static const RWTexture2D<float3> g_motionVectors = ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.OutMotionVectors];
static const RWTexture2D<float4> g_baseColorMetalness = ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.OutBaseColorMetalness];
static const RWTexture2D<float3> g_emissiveColor = ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.OutEmissiveColor];
static const RWTexture2D<float4> g_normalRoughness = ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.OutNormalRoughness];
static const RWTexture2D<float4> g_noisyDiffuse = ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.OutNoisyDiffuse];
static const RWTexture2D<float4> g_noisySpecular = ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.OutNoisySpecular];

inline float3 GetEnvironmentLightColor(float3 worldRayDirection) {
	if (g_globalResourceDescriptorHeapIndices.InEnvironmentLightCubeMap != ~0u) {
		return g_environmentLightCubeMap.SampleLevel(g_anisotropicSampler, normalize(STL::Geometry::RotateVector(g_sceneData.EnvironmentLightCubeMapTransform, worldRayDirection)), 0);
	}
	if (g_sceneData.EnvironmentLightColor.a >= 0) return g_sceneData.EnvironmentLightColor.rgb;
	return lerp(1, float3(0.5f, 0.7f, 1), (worldRayDirection.y + 1) * 0.5f);
}

inline bool GetEnvironmentColor(float3 worldRayDirection, out float3 color) {
	bool ret;
	if ((ret = g_globalResourceDescriptorHeapIndices.InEnvironmentCubeMap != ~0u)) {
		color = g_environmentCubeMap.SampleLevel(g_anisotropicSampler, normalize(STL::Geometry::RotateVector(g_sceneData.EnvironmentCubeMapTransform, worldRayDirection)), 0);
	}
	else if ((ret = g_sceneData.EnvironmentColor.a >= 0)) color = g_sceneData.EnvironmentColor.rgb;
	return ret;
}

inline float2 GetTextureCoordinate(uint objectIndex, uint primitiveIndex, float2 barycentrics) {
	const ObjectResourceDescriptorHeapIndices objectResourceDescriptorHeapIndices = g_objectResourceDescriptorHeapIndices[objectIndex];
	const StructuredBuffer<VertexPositionNormalTextureTangent> vertices = ResourceDescriptorHeap[objectResourceDescriptorHeapIndices.Mesh.Vertices];
	const uint3 indices = MeshHelpers::Load3Indices(ResourceDescriptorHeap[objectResourceDescriptorHeapIndices.Mesh.Indices], primitiveIndex);
	const float2 textureCoordinates[] = { vertices[indices[0]].TextureCoordinate, vertices[indices[1]].TextureCoordinate, vertices[indices[2]].TextureCoordinate };
	return Vertex::Interpolate(textureCoordinates, barycentrics);
}

inline float GetOpacity(uint objectIndex, float2 textureCoordinate, bool getBaseColorAlpha = true) {
	const ObjectResourceDescriptorHeapIndices objectResourceDescriptorHeapIndices = g_objectResourceDescriptorHeapIndices[objectIndex];

	uint index;
	if ((index = objectResourceDescriptorHeapIndices.Textures.TransmissionMap) != ~0u) {
		const Texture2D<float> texture = ResourceDescriptorHeap[index];
		return 1 - texture.SampleLevel(g_anisotropicSampler, textureCoordinate, 0);
	}
	if ((index = objectResourceDescriptorHeapIndices.Textures.OpacityMap) != ~0u) {
		const Texture2D<float> texture = ResourceDescriptorHeap[index];
		return texture.SampleLevel(g_anisotropicSampler, textureCoordinate, 0);
	}
	if (g_objectData[objectIndex].Material.AlphaMode != AlphaMode::Opaque && getBaseColorAlpha) {
		if ((index = objectResourceDescriptorHeapIndices.Textures.BaseColorMap) != ~0u) {
			const Texture2D<float4> texture = ResourceDescriptorHeap[index];
			return texture.SampleLevel(g_anisotropicSampler, textureCoordinate, 0).a;
		}
		return min(g_objectData[objectIndex].Material.Opacity, g_objectData[objectIndex].Material.BaseColor.a);
	}
	return g_objectData[objectIndex].Material.Opacity;
}

inline Material GetMaterial(uint objectIndex, float2 textureCoordinate) {
	const ObjectResourceDescriptorHeapIndices objectResourceDescriptorHeapIndices = g_objectResourceDescriptorHeapIndices[objectIndex];

	Material material;

	uint index;

	if ((index = objectResourceDescriptorHeapIndices.Textures.BaseColorMap) != ~0u) {
		const Texture2D<float4> texture = ResourceDescriptorHeap[index];
		material.BaseColor = texture.SampleLevel(g_anisotropicSampler, textureCoordinate, 0);
	}
	else material.BaseColor = g_objectData[objectIndex].Material.BaseColor;

	if ((index = objectResourceDescriptorHeapIndices.Textures.EmissiveColorMap) != ~0u) {
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
			metallicMapIndex = objectResourceDescriptorHeapIndices.Textures.MetallicMap,
			roughnessMapIndex = objectResourceDescriptorHeapIndices.Textures.RoughnessMap,
			ambientOcclusionMapIndex = objectResourceDescriptorHeapIndices.Textures.AmbientOcclusionMap;
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
	if (g_objectData[objectIndex].Material.AlphaMode != AlphaMode::Opaque) material.Opacity = min(material.Opacity, material.BaseColor.a);

	material.RefractiveIndex = max(g_objectData[objectIndex].Material.RefractiveIndex, 1);

	material.AlphaMode = g_objectData[objectIndex].Material.AlphaMode;
	material.AlphaThreshold = g_objectData[objectIndex].Material.AlphaThreshold;

	return material;
}

inline bool CastRay(RayDesc rayDesc, out HitInfo hitInfo) {
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
		hitInfo.Barycentrics = q.CommittedTriangleBarycentrics();
		hitInfo.Distance = q.CommittedRayT();
		hitInfo.InstanceIndex = q.CommittedInstanceIndex();
		hitInfo.ObjectIndex = q.CommittedGeometryIndex() + q.CommittedInstanceID();
		hitInfo.PrimitiveIndex = q.CommittedPrimitiveIndex();

		const ObjectResourceDescriptorHeapIndices objectResourceDescriptorHeapIndices = g_objectResourceDescriptorHeapIndices[hitInfo.ObjectIndex];
		const StructuredBuffer<VertexPositionNormalTexture> vertices = ResourceDescriptorHeap[objectResourceDescriptorHeapIndices.Mesh.Vertices];
		const uint3 indices = MeshHelpers::Load3Indices(ResourceDescriptorHeap[objectResourceDescriptorHeapIndices.Mesh.Indices], hitInfo.PrimitiveIndex);
		const float3
			positions[] = { vertices[indices[0]].Position, vertices[indices[1]].Position, vertices[indices[2]].Position },
			normals[] = { vertices[indices[0]].Normal, vertices[indices[1]].Normal, vertices[indices[2]].Normal };
		const float2 textureCoordinates[] = { vertices[indices[0]].TextureCoordinate, vertices[indices[1]].TextureCoordinate, vertices[indices[2]].TextureCoordinate };
		const float3 position = Vertex::Interpolate(positions, hitInfo.Barycentrics), normal = normalize(Vertex::Interpolate(normals, hitInfo.Barycentrics));

		hitInfo.ObjectPosition = position;
		hitInfo.Position = rayDesc.Origin + rayDesc.Direction * hitInfo.Distance;
		hitInfo.Normal = hitInfo.UnmappedNormal = normalize(STL::Geometry::RotateVector((float3x3)q.CommittedObjectToWorld3x4(), normal));
		hitInfo.TextureCoordinate = STL::Geometry::AffineTransform(g_objectData[hitInfo.ObjectIndex].TextureTransform, float3(Vertex::Interpolate(textureCoordinates, hitInfo.Barycentrics), 0)).xy;
		if (objectResourceDescriptorHeapIndices.Textures.NormalMap != ~0u) {
			const Texture2D<float3> texture = ResourceDescriptorHeap[objectResourceDescriptorHeapIndices.Textures.NormalMap];
			const float3 T = Math::CalculateTangent(positions, textureCoordinates);
			const float3x3 TBN = float3x3(T, normalize(cross(hitInfo.Normal, T)), hitInfo.Normal);
			hitInfo.Normal = normalize(STL::Geometry::RotateVectorInverse(TBN, texture.SampleLevel(g_anisotropicSampler, hitInfo.TextureCoordinate, 0) * 2 - 1));
		}
		hitInfo.SetFaceNormal(rayDesc.Direction);
	}
	return ret;
}

inline float3 CalculateMotionVector(float2 UV, float depth, float3 worldVertexPosition, float3 objectVertexPosition, uint instanceIndex, uint objectIndex, uint primitiveIndex, float2 barycentrics) {
	float3 previousPosition;
	if (g_sceneData.IsStatic || g_instanceData[instanceIndex].IsStatic) previousPosition = worldVertexPosition;
	else {
		previousPosition = objectVertexPosition;
		if (g_objectResourceDescriptorHeapIndices[objectIndex].Mesh.MotionVectors != ~0u) {
			const ObjectResourceDescriptorHeapIndices objectResourceDescriptorHeapIndices = g_objectResourceDescriptorHeapIndices[objectIndex];
			const StructuredBuffer<float3> meshMotionVectors = ResourceDescriptorHeap[objectResourceDescriptorHeapIndices.Mesh.MotionVectors];
			const uint3 indices = MeshHelpers::Load3Indices(ResourceDescriptorHeap[objectResourceDescriptorHeapIndices.Mesh.Indices], primitiveIndex);
			const float3 motionVectors[] = { meshMotionVectors[indices[0]], meshMotionVectors[indices[1]], meshMotionVectors[indices[2]] };
			previousPosition += Vertex::Interpolate(motionVectors, barycentrics);
		}
		previousPosition = STL::Geometry::AffineTransform(g_instanceData[instanceIndex].PreviousObjectToWorld, previousPosition);
	}
	return float3((STL::Geometry::GetScreenUv(g_camera.PreviousWorldToProjection, previousPosition) - UV) * g_renderSize, STL::Geometry::AffineTransform(g_camera.PreviousWorldToView, previousPosition).z - depth);
}
