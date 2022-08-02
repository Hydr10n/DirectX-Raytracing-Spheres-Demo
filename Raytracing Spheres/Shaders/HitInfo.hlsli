#pragma once

#include "Vertex.hlsli"

struct HitInfo {
	VertexPositionNormalTexture Vertex;
	bool IsFrontFace;

	void SetFaceNormal(float3 outwardNormal, float3 rayDirection) {
		IsFrontFace = dot(outwardNormal, rayDirection) < 0;
		Vertex.Normal = IsFrontFace ? outwardNormal : -outwardNormal;
	}

	void SetFaceNormal(float3 rayDirection) { SetFaceNormal(Vertex.Normal, rayDirection); }
};
