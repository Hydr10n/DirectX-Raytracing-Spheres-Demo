#pragma once

#include "Vertex.hlsli"

struct HitInfo {
	VertexPositionNormalTexture Vertex;
	float3 VertexUnmappedNormal;
	bool IsFrontFace;

	static bool SetFaceNormal(float3 rayDirection, inout float3 outwardNormal) {
		const bool isFrontFace = dot(outwardNormal, rayDirection) < 0;
		if (!isFrontFace) outwardNormal = -outwardNormal;
		return isFrontFace;
	}

	void SetFaceNormal(float3 rayDirection) { IsFrontFace = SetFaceNormal(rayDirection, Vertex.Normal); }
};
