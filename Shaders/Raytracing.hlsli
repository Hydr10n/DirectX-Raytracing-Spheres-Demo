#pragma once

#include "Common.hlsli"

#include "Math.hlsli"

#include "Camera.hlsli"

#include "MeshHelpers.hlsli"

#include "NRDDenoiser.hlsli"

SamplerState g_anisotropicSampler : register(s0);

RaytracingAccelerationStructure g_scene : register(t0);

struct NRDSettings {
	NRDDenoiser Denoiser;
	uint3 _;
	float4 HitDistanceParameters;
};

struct GraphicsSettings {
	uint2 RenderSize;
	uint FrameIndex, MaxNumberOfBounces, SamplesPerPixel;
	bool IsRussianRouletteEnabled;
	uint2 _;
	NRDSettings NRD;
};
ConstantBuffer<GraphicsSettings> g_graphicsSettings : register(b0);

ConstantBuffer<SceneData> g_sceneData : register(b1);

ConstantBuffer<Camera> g_camera : register(b2);

StructuredBuffer<InstanceData> g_instanceData : register(t1);
StructuredBuffer<ObjectData> g_objectData : register(t2);

RWTexture2D<float3> g_color : register(u0);
RWTexture2D<float> g_linearDepth : register(u1);
RWTexture2D<float> g_normalizedDepth : register(u2);
RWTexture2D<float3> g_motionVectors : register(u3);
RWTexture2D<float4> g_baseColorMetalness : register(u4);
RWTexture2D<float3> g_emissiveColor : register(u5);
RWTexture2D<float4> g_normalRoughness : register(u6);
RWTexture2D<float4> g_noisyDiffuse : register(u7);
RWTexture2D<float4> g_noisySpecular : register(u8);

inline float3 GetEnvironmentLightColor(float3 worldRayDirection) {
	const SceneResourceDescriptorHeapIndices resourceDescriptorHeapIndices = g_sceneData.ResourceDescriptorHeapIndices;
	if (resourceDescriptorHeapIndices.InEnvironmentLightTexture != ~0u) {
		worldRayDirection = normalize(STL::Geometry::RotateVector((float3x3)g_sceneData.EnvironmentLightTextureTransform, worldRayDirection));
		if (g_sceneData.IsEnvironmentLightTextureCubeMap) {
			const TextureCube<float3> texture = ResourceDescriptorHeap[resourceDescriptorHeapIndices.InEnvironmentLightTexture];
			return texture.SampleLevel(g_anisotropicSampler, worldRayDirection, 0);
		}
		const Texture2D<float3> texture = ResourceDescriptorHeap[resourceDescriptorHeapIndices.InEnvironmentLightTexture];
		return texture.SampleLevel(g_anisotropicSampler, Math::ToLatLongCoordinate(worldRayDirection), 0);
	}
	if (g_sceneData.EnvironmentLightColor.a >= 0) return g_sceneData.EnvironmentLightColor.rgb;
	return lerp(1, float3(0.5f, 0.7f, 1), (worldRayDirection.y + 1) * 0.5f);
}

inline bool GetEnvironmentColor(float3 worldRayDirection, out float3 color) {
	const SceneResourceDescriptorHeapIndices resourceDescriptorHeapIndices = g_sceneData.ResourceDescriptorHeapIndices;
	bool ret;
	if ((ret = resourceDescriptorHeapIndices.InEnvironmentTexture != ~0u)) {
		worldRayDirection = normalize(STL::Geometry::RotateVector((float3x3)g_sceneData.EnvironmentTextureTransform, worldRayDirection));
		if (g_sceneData.IsEnvironmentTextureCubeMap) {
			const TextureCube<float3> texture = ResourceDescriptorHeap[resourceDescriptorHeapIndices.InEnvironmentTexture];
			color = texture.SampleLevel(g_anisotropicSampler, worldRayDirection, 0);
		}
		else {
			const Texture2D<float3> texture = ResourceDescriptorHeap[resourceDescriptorHeapIndices.InEnvironmentTexture];
			color = texture.SampleLevel(g_anisotropicSampler, Math::ToLatLongCoordinate(worldRayDirection), 0);
		}
	}
	else if ((ret = g_sceneData.EnvironmentColor.a >= 0)) color = g_sceneData.EnvironmentColor.rgb;
	return ret;
}

inline float2 GetTextureCoordinate(uint objectIndex, uint primitiveIndex, float2 barycentrics) {
	const ObjectResourceDescriptorHeapIndices resourceDescriptorHeapIndices = g_objectData[objectIndex].ResourceDescriptorHeapIndices;
	const StructuredBuffer<VertexPositionNormalTexture> vertices = ResourceDescriptorHeap[resourceDescriptorHeapIndices.Mesh.Vertices];
	const uint3 indices = MeshHelpers::Load3Indices(ResourceDescriptorHeap[resourceDescriptorHeapIndices.Mesh.Indices], primitiveIndex);
	const float2 textureCoordinates[] = { vertices[indices[0]].TextureCoordinate, vertices[indices[1]].TextureCoordinate, vertices[indices[2]].TextureCoordinate };
	return Vertex::Interpolate(textureCoordinates, barycentrics);
}

inline float GetOpacity(uint objectIndex, float2 textureCoordinate, bool getBaseColorAlpha = true) {
	const ObjectResourceDescriptorHeapIndices resourceDescriptorHeapIndices = g_objectData[objectIndex].ResourceDescriptorHeapIndices;

	uint index;
	if ((index = resourceDescriptorHeapIndices.Textures.TransmissionMap) != ~0u) {
		const Texture2D<float> texture = ResourceDescriptorHeap[index];
		return 1 - texture.SampleLevel(g_anisotropicSampler, textureCoordinate, 0);
	}
	if ((index = resourceDescriptorHeapIndices.Textures.OpacityMap) != ~0u) {
		const Texture2D<float> texture = ResourceDescriptorHeap[index];
		return texture.SampleLevel(g_anisotropicSampler, textureCoordinate, 0);
	}
	if (g_objectData[objectIndex].Material.AlphaMode != AlphaMode::Opaque && getBaseColorAlpha) {
		if ((index = resourceDescriptorHeapIndices.Textures.BaseColorMap) != ~0u) {
			const Texture2D<float4> texture = ResourceDescriptorHeap[index];
			return texture.SampleLevel(g_anisotropicSampler, textureCoordinate, 0).a;
		}
		return min(g_objectData[objectIndex].Material.Opacity, g_objectData[objectIndex].Material.BaseColor.a);
	}
	return g_objectData[objectIndex].Material.Opacity;
}

inline Material GetMaterial(uint objectIndex, float2 textureCoordinate) {
	const ObjectResourceDescriptorHeapIndices resourceDescriptorHeapIndices = g_objectData[objectIndex].ResourceDescriptorHeapIndices;

	Material material;

	uint index;

	if ((index = resourceDescriptorHeapIndices.Textures.BaseColorMap) != ~0u) {
		const Texture2D<float4> texture = ResourceDescriptorHeap[index];
		material.BaseColor = texture.SampleLevel(g_anisotropicSampler, textureCoordinate, 0);
	}
	else material.BaseColor = g_objectData[objectIndex].Material.BaseColor;

	if ((index = resourceDescriptorHeapIndices.Textures.EmissiveColorMap) != ~0u) {
		const Texture2D<float3> texture = ResourceDescriptorHeap[index];
		material.EmissiveColor = texture.SampleLevel(g_anisotropicSampler, textureCoordinate, 0);
	}
	else material.EmissiveColor = g_objectData[objectIndex].Material.EmissiveColor;

	{
		/*
		 * glTF 2.0: Metallic (Red) | Roughness (Green)
		 * Others: Roughness (Green) | Metallic (Blue)
		 */

		const uint
			metallicMapIndex = resourceDescriptorHeapIndices.Textures.MetallicMap,
			roughnessMapIndex = resourceDescriptorHeapIndices.Textures.RoughnessMap,
			ambientOcclusionMapIndex = resourceDescriptorHeapIndices.Textures.AmbientOcclusionMap;
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
		material.Roughness = max(material.Roughness, MinRoughness);
	}

	material.Opacity = GetOpacity(objectIndex, textureCoordinate, false);
	if (g_objectData[objectIndex].Material.AlphaMode != AlphaMode::Opaque) material.Opacity = min(material.Opacity, material.BaseColor.a);

	material.RefractiveIndex = max(g_objectData[objectIndex].Material.RefractiveIndex, 1);

	material.AlphaMode = g_objectData[objectIndex].Material.AlphaMode;
	material.AlphaThreshold = g_objectData[objectIndex].Material.AlphaThreshold;

	return material;
}

inline float3 CalculateMotionVector(float2 UV, float linearDepth, HitInfo hitInfo) {
	float3 previousPosition;
	if (g_sceneData.IsStatic || g_instanceData[hitInfo.InstanceIndex].IsStatic) previousPosition = hitInfo.Position;
	else {
		previousPosition = hitInfo.ObjectPosition;
		const ObjectResourceDescriptorHeapIndices resourceDescriptorHeapIndices = g_objectData[hitInfo.ObjectIndex].ResourceDescriptorHeapIndices;
		if (resourceDescriptorHeapIndices.Mesh.MotionVectors != ~0u) {
			const StructuredBuffer<float3> meshMotionVectors = ResourceDescriptorHeap[resourceDescriptorHeapIndices.Mesh.MotionVectors];
			const uint3 indices = MeshHelpers::Load3Indices(ResourceDescriptorHeap[resourceDescriptorHeapIndices.Mesh.Indices], hitInfo.PrimitiveIndex);
			const float3 motionVectors[] = { meshMotionVectors[indices[0]], meshMotionVectors[indices[1]], meshMotionVectors[indices[2]] };
			previousPosition += Vertex::Interpolate(motionVectors, hitInfo.Barycentrics);
		}
		previousPosition = STL::Geometry::AffineTransform(g_instanceData[hitInfo.InstanceIndex].PreviousObjectToWorld, previousPosition);
	}
	return float3((STL::Geometry::GetScreenUv(g_camera.PreviousWorldToProjection, previousPosition) - UV) * g_graphicsSettings.RenderSize, STL::Geometry::AffineTransform(g_camera.PreviousWorldToView, previousPosition).z - linearDepth);
}

#define TRACE_RAY(q, rayDesc, flags, mask) \
	q.TraceRayInline(g_scene, flags, mask, rayDesc); \
	while (q.Proceed()) { \
		const uint objectIndex = g_instanceData[q.CandidateInstanceIndex()].FirstGeometryIndex + q.CandidateGeometryIndex(); \
		const AlphaMode alphaMode = g_objectData[objectIndex].Material.AlphaMode; \
		if (alphaMode == AlphaMode::Opaque) q.CommitNonOpaqueTriangleHit(); \
		else { /*TODO: Alpha Blending*/ \
			const float2 textureCoordinate = GetTextureCoordinate(objectIndex, q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics()); \
			const float opacity = GetOpacity(objectIndex, textureCoordinate); \
			if (opacity >= g_objectData[objectIndex].Material.AlphaThreshold) q.CommitNonOpaqueTriangleHit(); \
		} \
	}

inline bool CastRay(RayDesc rayDesc, out HitInfo hitInfo) {
	RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
	TRACE_RAY(q, rayDesc, RAY_FLAG_NONE, ~0u);
	const bool ret = q.CommittedStatus() != COMMITTED_NOTHING;
	if (ret) {
		hitInfo.InstanceIndex = q.CommittedInstanceIndex();
		hitInfo.ObjectIndex = g_instanceData[q.CommittedInstanceIndex()].FirstGeometryIndex + q.CommittedGeometryIndex();
		hitInfo.PrimitiveIndex = q.CommittedPrimitiveIndex();

		const ObjectResourceDescriptorHeapIndices resourceDescriptorHeapIndices = g_objectData[hitInfo.ObjectIndex].ResourceDescriptorHeapIndices;
		const StructuredBuffer<VertexPositionNormalTexture> vertices = ResourceDescriptorHeap[resourceDescriptorHeapIndices.Mesh.Vertices];
		const uint3 indices = MeshHelpers::Load3Indices(ResourceDescriptorHeap[resourceDescriptorHeapIndices.Mesh.Indices], hitInfo.PrimitiveIndex);
		const float3
			positions[] = { vertices[indices[0]].Position, vertices[indices[1]].Position, vertices[indices[2]].Position },
			normals[] = { vertices[indices[0]].Normal, vertices[indices[1]].Normal, vertices[indices[2]].Normal };
		const float2 textureCoordinates[] = { vertices[indices[0]].TextureCoordinate, vertices[indices[1]].TextureCoordinate, vertices[indices[2]].TextureCoordinate };
		hitInfo.Initialize(positions, normals, textureCoordinates, q.CommittedTriangleBarycentrics(), q.CommittedObjectToWorld3x4(), q.CommittedWorldToObject3x4(), rayDesc.Origin, rayDesc.Direction, q.CommittedRayT());
		if (resourceDescriptorHeapIndices.Textures.NormalMap != ~0u) {
			const Texture2D<float3> texture = ResourceDescriptorHeap[resourceDescriptorHeapIndices.Textures.NormalMap];
			const float3 T = normalize(Math::CalculateTangent(positions, textureCoordinates));
			const float3x3 TBN = float3x3(T, normalize(cross(hitInfo.Normal, T)), hitInfo.Normal);
			hitInfo.Normal = normalize(STL::Geometry::RotateVectorInverse(TBN, texture.SampleLevel(g_anisotropicSampler, hitInfo.TextureCoordinate, 0) * 2 - 1));
		}
	}
	return ret;
}
