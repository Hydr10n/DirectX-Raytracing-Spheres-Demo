#pragma once

#include "Camera.hlsli"

#include "Material.hlsli"

#include "HitInfo.hlsli"

#include "Math.hlsli"

#include "TriangleMeshIndexHelpers.hlsli"

SamplerState g_anisotropicWrap : register(s0);

RaytracingAccelerationStructure g_scene : register(t0);

struct GlobalResourceDescriptorHeapIndices {
	uint
		LocalResourceDescriptorHeapIndices,
		Camera,
		GlobalData, LocalData,
		Output,
		EnvironmentCubeMap;
	uint2 _padding;
};
ConstantBuffer<GlobalResourceDescriptorHeapIndices> g_globalResourceDescriptorHeapIndices : register(b0);

struct LocalResourceDescriptorHeapIndices {
	struct {
		uint Vertices, Indices;
		uint2 _padding;
	} TriangleMesh;
	struct {
		uint BaseColorMap, NormalMap;
		uint2 _padding;
	} Textures;
};
static const StructuredBuffer<LocalResourceDescriptorHeapIndices> g_localResourceDescriptorHeapIndices = ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.LocalResourceDescriptorHeapIndices];

static const ConstantBuffer<Camera> g_camera = ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.Camera];

struct GlobalData {
	uint RaytracingMaxTraceRecursionDepth, RaytracingSamplesPerPixel, FrameCount;
	uint _padding;
	float4x4 EnvironmentMapTransform;
};
static const ConstantBuffer<GlobalData> g_globalData = ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.GlobalData];

struct LocalData {
	float4x4 TextureTransform;
	Material Material;
};
static const StructuredBuffer<LocalData> g_localData = ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.LocalData];

static const RWTexture2D<float4> g_output = ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.Output];

static const TextureCube<float4> g_environmentCubeMap = ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.EnvironmentCubeMap];

inline Material GetMaterial(uint instanceID, float2 textureCoordinate) {
	Material material = g_localData[instanceID].Material;

	uint index;

	if ((index = g_localResourceDescriptorHeapIndices[instanceID].Textures.BaseColorMap) != ~0u) {
		const Texture2D<float4> texture = ResourceDescriptorHeap[index];
		material.BaseColor = texture.SampleLevel(g_anisotropicWrap, textureCoordinate, 0);
	}

	return material;
}

inline HitInfo GetHitInfo(uint instanceID, float3 worldRayOrigin, float3 worldRayDirection, float rayT, float4x3 objectToWorld, uint primitiveIndex, float2 barycentrics) {
	const LocalResourceDescriptorHeapIndices localResourceDescriptorHeapIndices = g_localResourceDescriptorHeapIndices[instanceID];

	const StructuredBuffer<VertexPositionNormalTexture> vertices = ResourceDescriptorHeap[localResourceDescriptorHeapIndices.TriangleMesh.Vertices];
	const uint3 indices = TriangleMeshIndexHelpers::Load3Indices(ResourceDescriptorHeap[localResourceDescriptorHeapIndices.TriangleMesh.Indices], primitiveIndex);
	const float3 normals[] = { vertices[indices[0]].Normal, vertices[indices[1]].Normal, vertices[indices[2]].Normal };
	const float2 textureCoordinates[] = { vertices[indices[0]].TextureCoordinate, vertices[indices[1]].TextureCoordinate, vertices[indices[2]].TextureCoordinate };

	HitInfo hitInfo;
	hitInfo.Vertex.Position = worldRayOrigin + worldRayDirection * rayT;
	hitInfo.Vertex.Normal = normalize(mul(Vertex::Interpolate(normals, barycentrics), (float3x3) objectToWorld));
	hitInfo.Vertex.TextureCoordinate = mul(float4(Vertex::Interpolate(textureCoordinates, barycentrics), 0, 1), g_localData[instanceID].TextureTransform).xy;
	if (localResourceDescriptorHeapIndices.Textures.NormalMap != ~0u) {
		const Texture2D<float3> normalMap = ResourceDescriptorHeap[localResourceDescriptorHeapIndices.Textures.NormalMap];
		const float3x3 TBN = Math::CalculateTBN(hitInfo.Vertex.Normal, worldRayDirection);
		hitInfo.Vertex.Normal = normalize(mul(normalize(normalMap.SampleLevel(g_anisotropicWrap, hitInfo.Vertex.TextureCoordinate, 0) * 2 - 1), TBN));
	}
	hitInfo.SetFaceNormal(worldRayDirection);
	return hitInfo;
}
