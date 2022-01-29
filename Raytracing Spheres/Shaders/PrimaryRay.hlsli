#ifndef PRIMARYRAY_HLSLI
#define PRIMARYRAY_HLSLI

#include "Common.hlsli"

#include "Ray.hlsli"

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
	if (!payload.TraceRecursionDepth) {
		payload.Color = 0;
		return;
	}

	const uint3 indices = Load3x16BitIndices(g_indices, GetTriangleBaseIndex(2));

	const RayDesc ray = CreateRayDesc(WorldRayOrigin(), WorldRayDirection());

	const float3 normals[] = { g_vertices[indices[0]].Normal, g_vertices[indices[1]].Normal, g_vertices[indices[2]].Normal };
	HitRecord hitRecord;
	hitRecord.Vertex.Position = ray.Origin + ray.Direction * RayTCurrent();
	hitRecord.SetFaceNormal(ray.Direction, normalize(mul(VertexAttribute(normals, attributes), (float3x3) ObjectToWorld4x3())));

	float4 color;
	if (g_objectConstant.IsImageTextureUsed) {
		const float2 textureCoordinates[] = { g_vertices[indices[0]].TextureCoordinate, g_vertices[indices[1]].TextureCoordinate, g_vertices[indices[2]].TextureCoordinate };
		float2 textureCoordinate = VertexAttribute(textureCoordinates, attributes);
		textureCoordinate.x = 1 - textureCoordinate.x;
		color = g_imageTexture.SampleLevel(g_anisotropicWrap, textureCoordinate, 0);
	}
	else color = g_objectConstant.Material.Color;

	float3 direction;
	float4 emitted = 0, attenuation = 0;
	if (g_objectConstant.Material.Emit()) emitted = color;
	if (g_objectConstant.Material.Scatter(ray, hitRecord, direction, payload.Random)) attenuation = color;

	payload.Color = emitted + attenuation * TracePrimaryRay(CreateRayDesc(hitRecord.Vertex.Position, direction), payload.TraceRecursionDepth, payload.Random);
}

TriangleHitGroup PrimaryRayHitGroup = { "", "PrimaryRayClosestHit" };

SubobjectToExportsAssociation PrimaryRayLocalRootSignatureAssociation = { "LocalRootSignature", "PrimaryRayHitGroup" };

#endif
