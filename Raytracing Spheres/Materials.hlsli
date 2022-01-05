#ifndef MATERIALS_HLSLI
#define MATERIALS_HLSLI

#include "HitRecord.hlsli"

#include "Random.hlsli"

inline float SchlickReflectance(float cosine, float refractiveIndex) {
	float r0 = (1 - refractiveIndex) / (1 + refractiveIndex);
	r0 *= r0;
	return r0 + (1 - r0) * pow(1 - cosine, 5);
}

interface IMaterial { bool Scatter(RayDesc ray, HitRecord hitRecord, out float3 direction, inout Random random); };

struct Lambertian : IMaterial
{
	bool Scatter(RayDesc ray, HitRecord hitRecord, out float3 direction, inout Random random) {
		direction = hitRecord.Vertex.Normal + random.UnitVector();

		// Catch degenerate scatter direction
		const float O = 1e-8;
		if (abs(direction[0]) < O && abs(direction[1]) < O && abs(direction[2]) < O) direction = hitRecord.Vertex.Normal;

		return true;
	}
};

struct Metal : IMaterial {
	float Roughness;

	bool Scatter(RayDesc ray, HitRecord hitRecord, out float3 direction, inout Random random) {
		const float3 unitDirection = normalize(ray.Direction);
		direction = reflect(unitDirection, hitRecord.Vertex.Normal) + Roughness * random.InUnitSphere();

		return dot(direction, hitRecord.Vertex.Normal) > 0;
	}
};

struct Dielectric : IMaterial {
	float RefractiveIndex;

	bool Scatter(RayDesc ray, HitRecord hitRecord, out float3 direction, inout Random random) {
		const float3 unitDirection = normalize(ray.Direction);
		const float cosTheta = dot(-unitDirection, hitRecord.Vertex.Normal), sinTheta = sqrt(1 - cosTheta * cosTheta);
		const float refractionRatio = hitRecord.IsFrontFace ? 1 / RefractiveIndex : RefractiveIndex;
		const bool canRefract = refractionRatio * sinTheta <= 1;
		direction = canRefract && SchlickReflectance(cosTheta, refractionRatio) < random.Float3() ? refract(unitDirection, hitRecord.Vertex.Normal, refractionRatio) : reflect(unitDirection, hitRecord.Vertex.Normal);

		return true;
	}
};

#endif
