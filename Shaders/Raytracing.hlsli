#pragma once

#include "Common.hlsli"

#include "Math.hlsli"

#include "Camera.hlsli"

#include "MeshHelpers.hlsli"

#define RTXDI_ENABLE_PRESAMPLING 0
#include "rtxdi/RtxdiParameters.h"

#include "Denoiser.hlsli"

SamplerState g_anisotropicSampler : register(s0);

RaytracingAccelerationStructure g_scene : register(t0);

struct RTXDISettings {
	bool IsEnabled;
	uint LocalLightSamples, BRDFSamples, SpatioTemporalSamples, InputBufferIndex, OutputBufferIndex, UniformRandomNumber;
	uint _;
	RTXDI_LightBufferParameters LightBufferParameters;
	RTXDI_RuntimeParameters RuntimeParameters;
	RTXDI_ReservoirBufferParameters ReservoirBufferParameters;
};

struct NRDSettings {
	NRDDenoiser Denoiser;
	uint3 _;
	float4 HitDistanceParameters;
};

struct GraphicsSettings {
	uint2 RenderSize;
	uint FrameIndex, Bounces, SamplesPerPixel;
	bool IsRussianRouletteEnabled;
	uint2 _;
	RTXDISettings RTXDI;
	NRDSettings NRD;
};
ConstantBuffer<GraphicsSettings> g_graphicsSettings : register(b0);

ConstantBuffer<SceneData> g_sceneData : register(b1);

ConstantBuffer<Camera> g_camera : register(b2);

StructuredBuffer<InstanceData> g_instanceData : register(t1);
StructuredBuffer<ObjectData> g_objectData : register(t2);

Texture2D<float> g_previousLinearDepth : register(t3);
Texture2D<float4> g_previousBaseColorMetalness : register(t4);
Texture2D<float4> g_previousNormalRoughness : register(t5);
Texture2D<float2> g_previousGeometricNormals : register(t6);

RWTexture2D<float3> g_color : register(u0);
RWTexture2D<float> g_linearDepth : register(u1);
RWTexture2D<float> g_normalizedDepth : register(u2);
RWTexture2D<float3> g_motionVectors : register(u3);
RWTexture2D<float4> g_baseColorMetalness : register(u4);
RWTexture2D<float3> g_emissiveColor : register(u5);
RWTexture2D<float4> g_normalRoughness : register(u6);
RWTexture2D<float2> g_geometricNormals : register(u7);
RWTexture2D<float4> g_noisyDiffuse : register(u8);
RWTexture2D<float4> g_noisySpecular : register(u9);

float3 GetEnvironmentLightColor(float3 worldRayDirection) {
	const SceneResourceDescriptorIndices resourceDescriptorIndices = g_sceneData.ResourceDescriptorIndices;
	if (resourceDescriptorIndices.InEnvironmentLightTexture != ~0u) {
		worldRayDirection = normalize(STL::Geometry::RotateVector((float3x3)g_sceneData.EnvironmentLightTextureTransform, worldRayDirection));
		if (g_sceneData.IsEnvironmentLightTextureCubeMap) {
			const TextureCube<float3> texture = ResourceDescriptorHeap[resourceDescriptorIndices.InEnvironmentLightTexture];
			return texture.SampleLevel(g_anisotropicSampler, worldRayDirection, 0);
		}
		const Texture2D<float3> texture = ResourceDescriptorHeap[resourceDescriptorIndices.InEnvironmentLightTexture];
		return texture.SampleLevel(g_anisotropicSampler, Math::ToLatLongCoordinate(worldRayDirection), 0);
	}
	if (g_sceneData.EnvironmentLightColor.a >= 0) return g_sceneData.EnvironmentLightColor.rgb;
	return lerp(1, float3(0.5f, 0.7f, 1), (worldRayDirection.y + 1) * 0.5f);
}

bool GetEnvironmentColor(float3 worldRayDirection, out float3 color) {
	const SceneResourceDescriptorIndices resourceDescriptorIndices = g_sceneData.ResourceDescriptorIndices;
	bool ret;
	if ((ret = resourceDescriptorIndices.InEnvironmentTexture != ~0u)) {
		worldRayDirection = normalize(STL::Geometry::RotateVector((float3x3)g_sceneData.EnvironmentTextureTransform, worldRayDirection));
		if (g_sceneData.IsEnvironmentTextureCubeMap) {
			const TextureCube<float3> texture = ResourceDescriptorHeap[resourceDescriptorIndices.InEnvironmentTexture];
			color = texture.SampleLevel(g_anisotropicSampler, worldRayDirection, 0);
		}
		else {
			const Texture2D<float3> texture = ResourceDescriptorHeap[resourceDescriptorIndices.InEnvironmentTexture];
			color = texture.SampleLevel(g_anisotropicSampler, Math::ToLatLongCoordinate(worldRayDirection), 0);
		}
	}
	else if ((ret = g_sceneData.EnvironmentColor.a >= 0)) color = g_sceneData.EnvironmentColor.rgb;
	return ret;
}

float2 GetTextureCoordinate(uint objectIndex, uint primitiveIndex, float2 barycentrics) {
	const ObjectResourceDescriptorIndices resourceDescriptorIndices = g_objectData[objectIndex].ResourceDescriptorIndices;
	const uint3 indices = MeshHelpers::Load3Indices(ResourceDescriptorHeap[resourceDescriptorIndices.Mesh.Indices], primitiveIndex);
	float2 textureCoordinates[3];
	g_objectData[objectIndex].VertexDesc.LoadTextureCoordinates(ResourceDescriptorHeap[resourceDescriptorIndices.Mesh.Vertices], indices, textureCoordinates);
	return Vertex::Interpolate(textureCoordinates, barycentrics);
}

float GetOpacity(uint objectIndex, float2 textureCoordinate, bool getBaseColorAlpha = true) {
	const ObjectResourceDescriptorIndices resourceDescriptorIndices = g_objectData[objectIndex].ResourceDescriptorIndices;

	uint index;
	if ((index = resourceDescriptorIndices.TextureMaps.Transmission) != ~0u) {
		const Texture2D<float> texture = ResourceDescriptorHeap[index];
		return 1 - texture.SampleLevel(g_anisotropicSampler, textureCoordinate, 0);
	}
	if ((index = resourceDescriptorIndices.TextureMaps.Opacity) != ~0u) {
		const Texture2D<float> texture = ResourceDescriptorHeap[index];
		return texture.SampleLevel(g_anisotropicSampler, textureCoordinate, 0);
	}
	if (g_objectData[objectIndex].Material.AlphaMode != AlphaMode::Opaque && getBaseColorAlpha) {
		if ((index = resourceDescriptorIndices.TextureMaps.BaseColor) != ~0u) {
			const Texture2D<float4> texture = ResourceDescriptorHeap[index];
			return texture.SampleLevel(g_anisotropicSampler, textureCoordinate, 0).a;
		}
		return min(g_objectData[objectIndex].Material.Opacity, g_objectData[objectIndex].Material.BaseColor.a);
	}
	return g_objectData[objectIndex].Material.Opacity;
}

Material GetMaterial(uint objectIndex, float2 textureCoordinate) {
	const ObjectResourceDescriptorIndices resourceDescriptorIndices = g_objectData[objectIndex].ResourceDescriptorIndices;

	Material material;

	uint index;

	if ((index = resourceDescriptorIndices.TextureMaps.BaseColor) != ~0u) {
		const Texture2D<float4> texture = ResourceDescriptorHeap[index];
		material.BaseColor = texture.SampleLevel(g_anisotropicSampler, textureCoordinate, 0);
	}
	else material.BaseColor = g_objectData[objectIndex].Material.BaseColor;

	if ((index = resourceDescriptorIndices.TextureMaps.EmissiveColor) != ~0u) {
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
			metallicMapIndex = resourceDescriptorIndices.TextureMaps.Metallic,
			roughnessMapIndex = resourceDescriptorIndices.TextureMaps.Roughness,
			ambientOcclusionMapIndex = resourceDescriptorIndices.TextureMaps.AmbientOcclusion;
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

float3 CalculateMotionVector(float2 UV, float linearDepth, HitInfo hitInfo) {
	float3 previousPosition;
	if (g_sceneData.IsStatic) previousPosition = hitInfo.Position;
	else {
		previousPosition = hitInfo.ObjectPosition;
		const ObjectResourceDescriptorIndices resourceDescriptorIndices = g_objectData[hitInfo.ObjectIndex].ResourceDescriptorIndices;
		if (resourceDescriptorIndices.Mesh.MotionVectors != ~0u) {
			const StructuredBuffer<float3> meshMotionVectors = ResourceDescriptorHeap[resourceDescriptorIndices.Mesh.MotionVectors];
			const uint3 indices = MeshHelpers::Load3Indices(ResourceDescriptorHeap[resourceDescriptorIndices.Mesh.Indices], hitInfo.PrimitiveIndex);
			const float3 motionVectors[] = { meshMotionVectors[indices[0]], meshMotionVectors[indices[1]], meshMotionVectors[indices[2]] };
			previousPosition += Vertex::Interpolate(motionVectors, hitInfo.Barycentrics);
		}
		previousPosition = STL::Geometry::AffineTransform(g_instanceData[hitInfo.InstanceIndex].PreviousObjectToWorld, previousPosition);
	}
	return float3((STL::Geometry::GetScreenUv(g_camera.PreviousWorldToProjection, previousPosition) - UV) * g_graphicsSettings.RenderSize, STL::Geometry::AffineTransform(g_camera.PreviousWorldToView, previousPosition).z - linearDepth);
}

#define TRACE_RAY(rayQuery, rayDesc, flags, mask) \
	rayQuery.TraceRayInline(g_scene, flags, mask, rayDesc); \
	while (rayQuery.Proceed()) { \
		const uint objectIndex = g_instanceData[rayQuery.CandidateInstanceIndex()].FirstGeometryIndex + rayQuery.CandidateGeometryIndex(); \
		const AlphaMode alphaMode = g_objectData[objectIndex].Material.AlphaMode; \
		if (alphaMode == AlphaMode::Opaque) rayQuery.CommitNonOpaqueTriangleHit(); \
		else { /*TODO: Alpha Blending*/ \
			const float2 textureCoordinate = GetTextureCoordinate(objectIndex, rayQuery.CandidatePrimitiveIndex(), rayQuery.CandidateTriangleBarycentrics()); \
			const float opacity = GetOpacity(objectIndex, textureCoordinate); \
			if (opacity >= g_objectData[objectIndex].Material.AlphaThreshold) rayQuery.CommitNonOpaqueTriangleHit(); \
		} \
	}

bool CastRay(RayDesc rayDesc, out HitInfo hitInfo) {
	RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
	TRACE_RAY(q, rayDesc, RAY_FLAG_NONE, ~0u);
	const bool ret = q.CommittedStatus() != COMMITTED_NOTHING;
	if (ret) {
		hitInfo.InstanceIndex = q.CommittedInstanceIndex();
		hitInfo.ObjectIndex = g_instanceData[q.CommittedInstanceIndex()].FirstGeometryIndex + q.CommittedGeometryIndex();
		hitInfo.PrimitiveIndex = q.CommittedPrimitiveIndex();

		const ObjectResourceDescriptorIndices resourceDescriptorIndices = g_objectData[hitInfo.ObjectIndex].ResourceDescriptorIndices;
		const uint3 indices = MeshHelpers::Load3Indices(ResourceDescriptorHeap[resourceDescriptorIndices.Mesh.Indices], hitInfo.PrimitiveIndex);
		const ByteAddressBuffer vertices = ResourceDescriptorHeap[resourceDescriptorIndices.Mesh.Vertices];
		const VertexDesc vertexDesc = g_objectData[hitInfo.ObjectIndex].VertexDesc;
		float3 positions[3], normals[3];
		float2 textureCoordinates[3];
		vertexDesc.LoadPositions(vertices, indices, positions);
		vertexDesc.LoadNormals(vertices, indices, normals);
		vertexDesc.LoadTextureCoordinates(vertices, indices, textureCoordinates);
		hitInfo.Initialize(positions, normals, textureCoordinates, q.CommittedTriangleBarycentrics(), q.CommittedObjectToWorld3x4(), q.CommittedWorldToObject3x4(), rayDesc.Origin, rayDesc.Direction, q.CommittedRayT());
		if (resourceDescriptorIndices.TextureMaps.Normal != ~0u) {
			const Texture2D<float3> texture = ResourceDescriptorHeap[resourceDescriptorIndices.TextureMaps.Normal];
			float3 T;
			if (vertexDesc.TangentOffset == ~0u) T = normalize(Math::CalculateTangent(positions, textureCoordinates));
			else {
				float3 tangents[3];
				vertexDesc.LoadTangents(vertices, indices, tangents);
				T = normalize(Vertex::Interpolate(tangents, hitInfo.Barycentrics));
			}
			const float3x3 TBN = float3x3(T, normalize(cross(hitInfo.Normal, T)), hitInfo.Normal);
			hitInfo.Normal = normalize(STL::Geometry::RotateVectorInverse(TBN, texture.SampleLevel(g_anisotropicSampler, hitInfo.TextureCoordinate, 0) * 2 - 1));
		}
	}
	return ret;
}
