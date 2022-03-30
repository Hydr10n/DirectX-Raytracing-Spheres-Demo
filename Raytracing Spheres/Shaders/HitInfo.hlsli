#ifndef HITINFO_HLSLI
#define HITINFO_HLSLI

#include "VertexTypes.hlsli"

struct HitInfo {
	VertexPositionNormal Vertex;
	bool IsFrontFace;

	void SetFaceNormal(float3 rayDirection, float3 outwardNormal) {
		IsFrontFace = dot(rayDirection, outwardNormal) < 0;
		Vertex.Normal = IsFrontFace ? outwardNormal : -outwardNormal;
	}
};

#endif
