#ifndef PRIMARYRAY_HLSLI
#define PRIMARYRAY_HLSLI

#include "Common.hlsli"

#include "Ray.hlsli"

#include "Utils.hlsli"

struct PrimaryRayPayload {
	float4 Color;
	uint TraceRecursionDepth;
	Random Random;
};

inline float4 TracePrimaryRay(RayDesc ray, uint traceRecursionDepth, inout Random random) {
	PrimaryRayPayload payload = { (float4) 0, traceRecursionDepth - 1, random };
	TraceRay(g_scene, g_sceneConstant.IsLeftHandedCoordinateSystem ? RAY_FLAG_CULL_BACK_FACING_TRIANGLES : RAY_FLAG_CULL_FRONT_FACING_TRIANGLES, 0xFF, 0, 1, 0, ray, payload);
	return payload.Color;
}

[shader("miss")]
void PrimaryRayMiss(inout PrimaryRayPayload payload) {
	if (g_sceneConstant.IsEnvironmentCubeMapUsed) {
		float3 textureCoordinate = mul(WorldRayDirection(), (float3x3) g_sceneConstant.EnvironmentMapTransform);
		if (!g_sceneConstant.IsLeftHandedCoordinateSystem) textureCoordinate.x = -textureCoordinate.x;
		payload.Color = g_environmentCubeMap.SampleLevel(g_anisotropicWrap, textureCoordinate, 0);
	}
	else {
		payload.Color = lerp(float4(1, 1, 1, 1), float4(0.5, 0.7, 1, 1), 0.5 * normalize(WorldRayDirection()).y + 0.5);
	}
}

[shader("closesthit")]
void PrimaryRayClosestHit(inout PrimaryRayPayload payload, BuiltInTriangleIntersectionAttributes attributes) {
	if (!payload.TraceRecursionDepth) {
		payload.Color = 0;
		return;
	}

	const uint3 indices = Load3x16BitIndices(g_indices, GetTriangleBaseIndex(2));

	const RayDesc ray = CreateRayDesc(WorldRayOrigin(), WorldRayDirection());

	float3 worldNormal;
	float2 textureCoordinate;
	{
		const float3 normals[] = { g_vertices[indices[0]].Normal, g_vertices[indices[1]].Normal, g_vertices[indices[2]].Normal };
		const float2 textureCoordinates[] = { g_vertices[indices[0]].TextureCoordinate, g_vertices[indices[1]].TextureCoordinate, g_vertices[indices[2]].TextureCoordinate };

		const float3 normal = VertexAttribute(normals, attributes.barycentrics);
		worldNormal = normalize(mul(normal, (float3x3) ObjectToWorld4x3()));

		textureCoordinate = VertexAttribute(textureCoordinates, attributes.barycentrics);
		textureCoordinate = mul(float4(textureCoordinate, 0, 1), g_objectConstant.TextureTransform).xy;

		if (g_objectConstant.TextureFlags & TextureFlags::NormalMap) {
			const float3 positions[] = { g_vertices[indices[0]].Position, g_vertices[indices[1]].Position, g_vertices[indices[2]].Position };
			const float3 tangent = CalculateTangent(positions, textureCoordinates);
			const float3x3 TBN = float3x3(tangent, cross(tangent, worldNormal), worldNormal);

			worldNormal = normalize(mul(normalize(g_normalMap.SampleLevel(g_anisotropicWrap, textureCoordinate, 0) * 2 - 1), TBN));
		}
	}

	HitRecord hitRecord;
	hitRecord.Vertex.Position = ray.Origin + ray.Direction * RayTCurrent();
	hitRecord.SetFaceNormal(ray.Direction, worldNormal);

	float4 color;
	if (g_objectConstant.TextureFlags & TextureFlags::ColorMap) color = g_colorMap.SampleLevel(g_anisotropicWrap, textureCoordinate, 0);
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
