#ifndef HITRECORD_HLSLI
#define HITRECORD_HLSLI

#include "Vertex.hlsli"

struct HitRecord {
	Vertex Vertex;
	bool IsFrontFace;

	void SetFaceNormal(RayDesc ray, float3 outwardNormal) {
		IsFrontFace = dot(ray.Direction, outwardNormal) < 0;
		Vertex.Normal = IsFrontFace ? outwardNormal : -outwardNormal;
	}
};

#endif
