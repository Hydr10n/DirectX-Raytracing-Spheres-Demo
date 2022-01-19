#ifndef PRIMARYRAY_HLSLI
#define PRIMARYRAY_HLSLI

#include "Common.hlsli"

#include "Ray.hlsli"

#include "Materials.hlsli"

#include "Index.hlsli"

struct PrimaryRayPayload {
	float4 Color;
	uint TraceRecursionDepth;
	Random Random;
};

inline float4 TracePrimaryRay(RayDesc ray, uint traceRecursionDepth, inout Random random) {
	PrimaryRayPayload payload = { (float4) 0, traceRecursionDepth - 1, random };
	TraceRay(g_scene, RAY_FLAG_CULL_BACK_FACING_TRIANGLES, 0xFF, 0, 1, 0, ray, payload);
	return payload.Color;
}

[shader("miss")]
void PrimaryRayMiss(inout PrimaryRayPayload payload) {
	payload.Color = lerp(float4(1, 1, 1, 1), float4(0.5, 0.7, 1, 1), 0.5 * (normalize(WorldRayDirection()).y + 1));
}

[shader("closesthit")]
void PrimaryRayClosestHit(inout PrimaryRayPayload payload, BuiltInTriangleIntersectionAttributes attributes) {
	const float3 worldRayOrigin = WorldRayOrigin(), worldRayDirection = WorldRayDirection();

	const uint3 indices = Load3x16BitIndices(g_indices, GetTriangleBaseIndex(2));
	const float3 normals[] = { g_vertices[indices[0]].Normal, g_vertices[indices[1]].Normal, g_vertices[indices[2]].Normal };
	const float2 textureCoordinates[] = { g_vertices[indices[0]].TextureCoordinate, g_vertices[indices[1]].TextureCoordinate, g_vertices[indices[2]].TextureCoordinate };
	const Vertex vertex = {
		worldRayOrigin + worldRayDirection * RayTCurrent(),
		normalize(mul(VertexAttribute(normals, attributes), (float3x3) ObjectToWorld4x3())),
		VertexAttribute(textureCoordinates, attributes)
	};

	if (!payload.TraceRecursionDepth) {
		payload.Color = 0;
		return;
	}

	const RayDesc ray = CreateRayDesc(worldRayOrigin, worldRayDirection);

	HitRecord hitRecord;
	hitRecord.Vertex.Position = vertex.Position;
	hitRecord.SetFaceNormal(ray, vertex.Normal);

	float3 direction;
	bool isScattered;

	switch (g_materialConstant.Type) {
	case 0: {
		Lambertian lambertian;
		isScattered = lambertian.Scatter(ray, hitRecord, direction, payload.Random);
	}	break;

	case 1: {
		Metal metal = { g_materialConstant.Roughness };
		isScattered = metal.Scatter(ray, hitRecord, direction, payload.Random);
	}	break;

	case 2: {
		Dielectric dielectric = { g_materialConstant.RefractiveIndex };
		isScattered = dielectric.Scatter(ray, hitRecord, direction, payload.Random);
	}	break;

	default: payload.Color = 0; return;
	}

	if (!isScattered) {
		payload.Color = 0;
		return;
	}

	float4 color;
	if (g_objectConstant.IsImageTextureUsed) {
		const float2 textureCoordinate = { 1 - vertex.TextureCoordinate.x, vertex.TextureCoordinate.y };
		color = g_imageTexture.SampleLevel(g_anisotropicWrap, textureCoordinate, 0);
	}
	else color = g_materialConstant.Color;
	payload.Color = color * TracePrimaryRay(CreateRayDesc(vertex.Position, direction), payload.TraceRecursionDepth, payload.Random);
}

#endif
