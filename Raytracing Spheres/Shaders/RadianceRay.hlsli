#pragma once

#include "Common.hlsli"

#include "GeometryIndexHelpers.hlsli"

#include "MathHelpers.hlsli"

TriangleHitGroup RadianceRayHitGroup = { "", "RadianceRayClosestHit" };

struct RadianceRayPayload {
	float4 Color;
	uint TraceRecursionDepth;
	Random Random;
};

inline float4 TraceRadianceRay(RayDesc rayDesc, uint traceRecursionDepth, inout Random random) {
	RadianceRayPayload payload = { (float4) 0, traceRecursionDepth - 1, random };
	TraceRay(g_scene, RAY_FLAG_NONE, ~0, 0, 1, 0, rayDesc, payload);
	return payload.Color;
}

[shader("miss")]
void RadianceRayMiss(inout RadianceRayPayload payload : SV_RayPayload) {
	if (g_globalResourceDescriptorHeapIndices.EnvironmentCubeMap != UINT_MAX) {
		payload.Color = g_environmentCubeMap.SampleLevel(g_anisotropicWrap, mul(WorldRayDirection(), (float3x3) g_globalData.EnvironmentMapTransform), 0);
	}
	else payload.Color = lerp(float4(1, 1, 1, 1), float4(0.5f, 0.7f, 1, 1), 0.5f * normalize(WorldRayDirection()).y + 0.5f);
}

[shader("closesthit")]
void RadianceRayClosestHit(inout RadianceRayPayload payload : SV_RayPayload, BuiltInTriangleIntersectionAttributes attributes : SV_IntersectionAttributes) {
	if (!payload.TraceRecursionDepth) return;

	const uint instanceID = InstanceID();

	const LocalResourceDescriptorHeapIndices localResourceDescriptorHeapIndices = g_localResourceDescriptorHeapIndices[instanceID];
	const LocalData localData = g_localData[instanceID];

	const float3 worldRayDirection = WorldRayDirection();

	float3 worldNormal;
	float2 textureCoordinate;
	{
		const StructuredBuffer<VertexPositionNormalTexture> vertices = ResourceDescriptorHeap[localResourceDescriptorHeapIndices.Mesh.Vertices];
		const uint3 indices = GeometryIndexHelpers::Load3x16BitIndices(ResourceDescriptorHeap[localResourceDescriptorHeapIndices.Mesh.Indices], PrimitiveIndex());
		const float3 normals[] = { vertices[indices[0]].Normal, vertices[indices[1]].Normal, vertices[indices[2]].Normal };
		const float2 textureCoordinates[] = { vertices[indices[0]].TextureCoordinate, vertices[indices[1]].TextureCoordinate, vertices[indices[2]].TextureCoordinate };

		const float3 normal = Vertex::Interpolate(normals, attributes.barycentrics);
		worldNormal = normalize(mul(normal, (float3x3) ObjectToWorld4x3()));

		textureCoordinate = Vertex::Interpolate(textureCoordinates, attributes.barycentrics);
		textureCoordinate = mul(float4(textureCoordinate, 0, 1), localData.TextureTransform).xy;

		if (localResourceDescriptorHeapIndices.Textures.NormalMap != UINT_MAX) {
			const Texture2D<float3> normalMap = ResourceDescriptorHeap[localResourceDescriptorHeapIndices.Textures.NormalMap];
			const float3x3 TBN = MathHelpers::CalculateTBN(worldNormal, worldRayDirection);
			worldNormal = normalize(mul(normalize(normalMap.SampleLevel(g_anisotropicWrap, textureCoordinate, 0) * 2 - 1), TBN));
		}
	}

	HitInfo hitInfo;
	hitInfo.Vertex.Position = WorldRayOrigin() + worldRayDirection * RayTCurrent();
	hitInfo.SetFaceNormal(worldNormal, worldRayDirection);

	float4 color;
	if (localResourceDescriptorHeapIndices.Textures.ColorMap != UINT_MAX) {
		const Texture2D<float4> colorMap = ResourceDescriptorHeap[localResourceDescriptorHeapIndices.Textures.ColorMap];
		color = colorMap.SampleLevel(g_anisotropicWrap, textureCoordinate, 0);
	}
	else color = localData.Material.Color;

	float3 direction = 0;
	if (localData.Material.Scatter(worldRayDirection, hitInfo, direction, payload.Random)) {
		const Ray ray = { hitInfo.Vertex.Position, direction };
		payload.Color = color * TraceRadianceRay(ray.ToRayDesc(), payload.TraceRecursionDepth, payload.Random);
	}
	else payload.Color = localData.Material.IsEmissive() ? color : 0;
}
