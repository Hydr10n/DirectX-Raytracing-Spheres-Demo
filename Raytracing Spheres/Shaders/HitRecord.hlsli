#ifndef HITRECORD_HLSLI
#define HITRECORD_HLSLI

#include "Vertex.hlsli"

struct HitRecord {
	VertexPositionNormal Vertex;
	bool IsFrontFace;

	void SetFaceNormal(float3 rayDirection, float3 outwardNormal) {
		IsFrontFace = dot(rayDirection, outwardNormal) < 0;
		Vertex.Normal = IsFrontFace ? outwardNormal : -outwardNormal;
	}
};

#endif
