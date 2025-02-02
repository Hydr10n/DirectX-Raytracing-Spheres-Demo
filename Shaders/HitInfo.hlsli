#pragma once

#include "Vertex.hlsli"

#include "SelfIntersectionAvoidance.hlsli"

struct HitInfo
{
	float3 Position;
	float PositionOffset;
	float3 ObjectPosition;
	bool IsFrontFace;
	float Distance;
	float3 Normal, GeometricNormal;
	uint InstanceIndex;
	float3 FlatNormal;
	uint ObjectIndex;
	float2 TextureCoordinate, Barycentrics;
	uint PrimitiveIndex;

	void Initialize(
		float3 positions[3], float3 normals[3],
		float2 barycentrics,
		float3x4 objectToWorld, float3x4 worldToObject,
		float3 worldRayDirection, float distance
	)
	{
		float3 objectNormal;
		SelfIntersectionAvoidance::GetSafeTriangleSpawnPoint(
			ObjectPosition, Position, objectNormal, FlatNormal, PositionOffset,
			positions, barycentrics, objectToWorld, worldToObject
		);
		Normal = GeometricNormal = normalize(Geometry::RotateVector(transpose((float3x3)worldToObject), Vertex::Interpolate(normals, barycentrics)));
		if (!(IsFrontFace = dot(Normal, worldRayDirection) < 0))
		{
			Normal = -Normal;
		}
		Barycentrics = barycentrics;
		Distance = distance;
	}

	void Initialize(
		float3 position, float positionOffset,
		float3 flatNormal, float3 normal, float3 geometricNormal,
		float3 worldRayDirection, float distance
	)
	{
		Position = position;
		PositionOffset = positionOffset;
		FlatNormal = flatNormal;
		Normal = normal;
		GeometricNormal = geometricNormal;
		IsFrontFace = dot(geometricNormal, worldRayDirection) < 0;
		Distance = distance;
	}

	float3 GetSafeWorldRayOrigin(float3 worldRayDirection)
	{
		return SelfIntersectionAvoidance::OffsetSpawnPoint(Position, FlatNormal * Math::Sign(dot(worldRayDirection, FlatNormal)), PositionOffset);
	}
};
