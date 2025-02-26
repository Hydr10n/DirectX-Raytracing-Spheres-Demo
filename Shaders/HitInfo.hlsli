#pragma once

#include "Vertex.hlsli"

#include "SelfIntersectionAvoidance.hlsli"

struct HitInfo
{
	float3 Position, ObjectPosition;
	float PositionOffset;

	float3 FlatNormal, GeometricNormal, ShadingNormal, Tangent;
	bool IsFrontFace;

	float2 TextureCoordinates[2];

	float2 Barycentrics;

	float Distance;

	uint InstanceIndex, ObjectIndex, PrimitiveIndex;

	void Initialize(
		float3 positions[3],
		float2 barycentrics,
		float3x4 objectToWorld, float3x4 worldToObject
	)
	{
		float3 objectNormal;
		SelfIntersectionAvoidance::GetSafeTriangleSpawnPoint(
			ObjectPosition, Position, objectNormal, FlatNormal, PositionOffset,
			positions, barycentrics, objectToWorld, worldToObject
		);
		Barycentrics = barycentrics;
	}

	void Initialize(
		float3 positions[3],
		float2 barycentrics,
		float3x4 objectToWorld, float3x4 worldToObject,
		float3 worldRayDirection
	)
	{
		Initialize(positions, barycentrics, objectToWorld, worldToObject);
		ShadingNormal = GeometricNormal = FlatNormal;
		if (!(IsFrontFace = dot(GeometricNormal, worldRayDirection) < 0))
		{
			ShadingNormal = -ShadingNormal;
		}
	}

	void Initialize(
		float3 positions[3], float3 normals[3],
		float2 barycentrics,
		float3x4 objectToWorld, float3x4 worldToObject,
		float3 worldRayDirection
	)
	{
		Initialize(positions, barycentrics, objectToWorld, worldToObject);
		ShadingNormal = GeometricNormal = normalize(Geometry::RotateVectorInverse((float3x3)worldToObject, Vertex::Interpolate(normals, barycentrics)));
		if (!(IsFrontFace = dot(GeometricNormal, worldRayDirection) < 0))
		{
			ShadingNormal = -ShadingNormal;
		}
	}

	void Initialize(
		float3 position, float positionOffset,
		float3 flatNormal, float3 geometricNormal, float3 shadingNormal,
		float3 worldRayDirection
	)
	{
		Position = position;
		PositionOffset = positionOffset;
		FlatNormal = flatNormal;
		GeometricNormal = geometricNormal;
		ShadingNormal = shadingNormal;
		IsFrontFace = dot(geometricNormal, worldRayDirection) < 0;
	}

	float3 GetFrontFlatNormal()
	{
		return IsFrontFace ? FlatNormal : -FlatNormal;
	}
	
	float3 GetFrontGeometricNormal()
	{
		return IsFrontFace ? GeometricNormal : -GeometricNormal;
	}

	float3 GetFrontTangent()
	{
		return IsFrontFace ? Tangent : -Tangent;
	}

	float3 GetSafeWorldRayOrigin(float3 worldRayDirection)
	{
		return SelfIntersectionAvoidance::OffsetSpawnPoint(Position, FlatNormal * Math::Sign(dot(worldRayDirection, FlatNormal)), PositionOffset);
	}
};
