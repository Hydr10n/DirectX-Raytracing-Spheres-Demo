#pragma once

#include "Vertex.hlsli"

struct HitInfo : VertexPositionNormalTexture {
	float3 ObjectPosition, UnmappedNormal;
	float2 Barycentrics;
	bool IsFrontFace;

	float Distance;

	uint InstanceIndex, ObjectIndex, PrimitiveIndex;

	static bool SetFaceNormal(float3 rayDirection, inout float3 outwardNormal) {
		const bool isFrontFace = dot(outwardNormal, rayDirection) < 0;
		if (!isFrontFace) outwardNormal = -outwardNormal;
		return isFrontFace;
	}

	void SetFaceNormal(float3 rayDirection) {
		IsFrontFace = SetFaceNormal(rayDirection, Normal);
		SetFaceNormal(rayDirection, UnmappedNormal);
	}
};
