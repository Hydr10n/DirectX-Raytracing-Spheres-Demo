#ifndef HITINFO_HLSLI
#define HITINFO_HLSLI

#include "Vertex.hlsli"

struct HitInfo {
	VertexPositionNormal Vertex;
	bool IsFrontFace;

	void SetFaceNormal(float3 outwardNormal, bool isFrontFace = HitKind() == HIT_KIND_TRIANGLE_FRONT_FACE) {
		Vertex.Normal = isFrontFace ? outwardNormal : -outwardNormal;
		IsFrontFace = isFrontFace;
	}
};

#endif
