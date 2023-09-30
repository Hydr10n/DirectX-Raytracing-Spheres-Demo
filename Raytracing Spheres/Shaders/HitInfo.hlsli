#pragma once

#include "Vertex.hlsli"

#include "STL.hlsli"

#include "SelfIntersectionAvoidance.hlsli"

struct HitInfo : VertexPositionNormalTexture {
	float3 ObjectPosition, UnmappedNormal;

	float3 SafePosition, SafeNormal;
	float SafeOffset;

	float2 Barycentrics;

	bool IsFrontFace;

	float Distance;

	uint InstanceIndex, ObjectIndex, PrimitiveIndex;

	void Initialize(float3 positions[3], float3 normals[3], float2 textureCoordinates[3], float2 barycentrics, float3x4 objectToWorld, float3x4 worldToObject, float3 worldRayOrigin, float3 worldRayDirection, float distance) {
		float3 objectNormal;
		SelfIntersectionAvoidance::GetSafeTriangleSpawnPoint(ObjectPosition, SafePosition, objectNormal, SafeNormal, SafeOffset, positions, barycentrics, objectToWorld, worldToObject);
		Position = worldRayOrigin + worldRayDirection * distance;
		Normal = normalize(STL::Geometry::RotateVector(transpose((float3x3)worldToObject), Vertex::Interpolate(normals, barycentrics)));
		IsFrontFace = dot(Normal, worldRayDirection) < 0;
		if (!IsFrontFace) Normal = -Normal;
		UnmappedNormal = Normal;
		TextureCoordinate = Vertex::Interpolate(textureCoordinates, barycentrics);
		Barycentrics = barycentrics;
		Distance = distance;
	}

	float3 GetSafeWorldRayOrigin(float3 worldRayDirection) { return SelfIntersectionAvoidance::OffsetSpawnPoint(SafePosition, SafeNormal * STL::Math::Sign(dot(worldRayDirection, SafeNormal)), SafeOffset); }
};
