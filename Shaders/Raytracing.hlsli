#pragma once

#include "Common.hlsli"

#include "Camera.hlsli"

#include "Denoiser.hlsli"

#if defined(SHARC_UPDATE) || defined(SHARC_QUERY)
#define ENABLE_SHARC 1
#if SHARC_UPDATE
#define SHARC_QUERY 0
#else
#define SHARC_UPDATE 0
#endif
#include "SharcCommon.h"
#else
#define ENABLE_SHARC 0
#define SHARC_QUERY 0
#define SHARC_UPDATE 0
#endif

#define NV_SHADER_EXTN_SLOT u1024
#include "nvHLSLExtns.h"

SamplerState g_anisotropicSampler : register(s0);

RaytracingAccelerationStructure g_scene : register(t0);

struct GraphicsSettings
{
	uint FrameIndex, Bounces, SamplesPerPixel;
	float ThroughputThreshold;
	bool IsRussianRouletteEnabled, IsShaderExecutionReorderingEnabled;
	uint2 _;
	struct
	{
		struct
		{
			uint Capacity;
			float SceneScale, RoughnessThreshold;
			bool IsHashGridVisualizationEnabled;
		} SHARC;
	} RTXGI;
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
RWTexture2D<float4> g_normals : register(u6);
RWTexture2D<float> g_roughness : register(u7);
RWTexture2D<float4> g_normalRoughness : register(u8);
RWTexture2D<float4> g_noisyDiffuse : register(u9);
RWTexture2D<float4> g_noisySpecular : register(u10);

#if ENABLE_SHARC
RWStructuredBuffer<uint64_t> g_sharcHashEntries : register(u11);
RWStructuredBuffer<uint4> g_sharcPreviousVoxelData : register(u12);
RWStructuredBuffer<uint4> g_sharcVoxelData : register(u13);
#endif

#include "RaytracingHelpers.hlsli"

float3 GetEnvironmentLightColor(float3 worldRayDirection)
{
	const SceneResourceDescriptorIndices resourceDescriptorIndices = g_sceneData.ResourceDescriptorIndices;
	if (resourceDescriptorIndices.InEnvironmentLightTexture != ~0u)
	{
		worldRayDirection = normalize(Geometry::RotateVector((float3x3)g_sceneData.EnvironmentLightTextureTransform, worldRayDirection));
		if (g_sceneData.IsEnvironmentLightTextureCubeMap)
		{
			const TextureCube<float3> texture = ResourceDescriptorHeap[resourceDescriptorIndices.InEnvironmentLightTexture];
			return texture.SampleLevel(g_anisotropicSampler, worldRayDirection, 0);
		}
		const Texture2D<float3> texture = ResourceDescriptorHeap[resourceDescriptorIndices.InEnvironmentLightTexture];
		return texture.SampleLevel(g_anisotropicSampler, Math::ToLatLongCoordinate(worldRayDirection), 0);
	}
	if (g_sceneData.EnvironmentLightColor.a >= 0)
	{
		return g_sceneData.EnvironmentLightColor.rgb;
	}
	return lerp(1, float3(0.5f, 0.7f, 1), (worldRayDirection.y + 1) * 0.5f);
}

bool GetEnvironmentColor(float3 worldRayDirection, out float3 color)
{
	const SceneResourceDescriptorIndices resourceDescriptorIndices = g_sceneData.ResourceDescriptorIndices;
	bool ret;
	if ((ret = resourceDescriptorIndices.InEnvironmentTexture != ~0u))
	{
		worldRayDirection = normalize(Geometry::RotateVector((float3x3)g_sceneData.EnvironmentTextureTransform, worldRayDirection));
		if (g_sceneData.IsEnvironmentTextureCubeMap)
		{
			const TextureCube<float3> texture = ResourceDescriptorHeap[resourceDescriptorIndices.InEnvironmentTexture];
			color = texture.SampleLevel(g_anisotropicSampler, worldRayDirection, 0);
		}
		else
		{
			const Texture2D<float3> texture = ResourceDescriptorHeap[resourceDescriptorIndices.InEnvironmentTexture];
			color = texture.SampleLevel(g_anisotropicSampler, Math::ToLatLongCoordinate(worldRayDirection), 0);
		}
	}
	else if ((ret = g_sceneData.EnvironmentColor.a >= 0))
	{
		color = g_sceneData.EnvironmentColor.rgb;
	}
	return ret;
}

float3 CalculateMotionVector(float2 UV, uint2 pixelDimensions, float linearDepth, HitInfo hitInfo)
{
	float3 previousPosition;
	if (g_sceneData.IsStatic)
	{
		previousPosition = hitInfo.Position;
	}
	else
	{
		previousPosition = hitInfo.ObjectPosition;
		const ObjectResourceDescriptorIndices resourceDescriptorIndices = g_objectData[hitInfo.ObjectIndex].ResourceDescriptorIndices;
		if (resourceDescriptorIndices.Mesh.MotionVectors != ~0u)
		{
			const StructuredBuffer<float3> meshMotionVectors = ResourceDescriptorHeap[resourceDescriptorIndices.Mesh.MotionVectors];
			const uint3 indices = MeshHelpers::Load3Indices(ResourceDescriptorHeap[resourceDescriptorIndices.Mesh.Indices], hitInfo.PrimitiveIndex);
			const float3 motionVectors[] = { meshMotionVectors[indices[0]], meshMotionVectors[indices[1]], meshMotionVectors[indices[2]] };
			previousPosition += Vertex::Interpolate(motionVectors, hitInfo.Barycentrics);
		}
		previousPosition = Geometry::AffineTransform(g_instanceData[hitInfo.InstanceIndex].PreviousObjectToWorld, previousPosition);
	}
	return float3((Geometry::GetScreenUv(g_camera.PreviousWorldToProjection, previousPosition) - UV) * pixelDimensions, Geometry::AffineTransform(g_camera.PreviousWorldToView, previousPosition).z - linearDepth);
}
