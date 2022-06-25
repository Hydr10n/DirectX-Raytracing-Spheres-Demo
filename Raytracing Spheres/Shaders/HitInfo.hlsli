#pragma once

#include "Vertex.hlsli"

struct HitInfo {
	VertexPositionNormal Vertex;
	bool IsFrontFace;

	void SetFaceNormal(float3 outwardNormal, float3 rayDirection) {
		IsFrontFace = dot(outwardNormal, rayDirection) < 0;
		Vertex.Normal = IsFrontFace ? outwardNormal : -outwardNormal;
	}
};
