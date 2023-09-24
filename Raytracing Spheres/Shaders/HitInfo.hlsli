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

	void Initialize(float3 positions[3], float3 normals[3], float2 textureCoordinates[3], float2 barycentrics, float3x4 objectToWorld, float3x4 worldToObject, float3 worldRayOrigin, float3 worldRayDirection, float distance, bool setFaceNormal = true) {
		Barycentrics = barycentrics;
		Distance = distance;
		ObjectPosition = Vertex::Interpolate(positions, barycentrics);
		Position = worldRayOrigin + worldRayDirection * distance;
		Normal = UnmappedNormal = normalize(STL::Geometry::RotateVector(transpose((float3x3)worldToObject), Vertex::Interpolate(normals, barycentrics)));
		if (setFaceNormal) SetFaceNormal(worldRayDirection);
		TextureCoordinate = Vertex::Interpolate(textureCoordinates, barycentrics);

		float3 objectPosition, objectNormal;
		SelfIntersectionAvoidance::GetSafeTriangleSpawnOffset(objectPosition, SafePosition, objectNormal, SafeNormal, SafeOffset, positions[0], positions[1], positions[2], barycentrics, objectToWorld, worldToObject);
	}

	static bool SetFaceNormal(float3 worldRayDirection, inout float3 normal) {
		const bool isFrontFace = dot(normal, worldRayDirection) < 0;
		if (!isFrontFace) normal = -normal;
		return isFrontFace;
	}

	void SetFaceNormal(float3 worldRayDirection) {
		IsFrontFace = SetFaceNormal(worldRayDirection, Normal);
		SetFaceNormal(worldRayDirection, UnmappedNormal);
	}

	float3 GetSafeWorldRayOrigin(float3 worldRayDirection) { return SelfIntersectionAvoidance::OffsetSpawnPoint(SafePosition, SafeNormal * STL::Math::Sign(dot(worldRayDirection, SafeNormal)), SafeOffset); }
};
