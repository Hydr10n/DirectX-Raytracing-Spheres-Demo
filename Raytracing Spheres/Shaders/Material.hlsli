#ifndef MATERIAL_HLSLI
#define MATERIAL_HLSLI

#include "HitRecord.hlsli"

#include "Random.hlsli"

inline float SchlickReflectance(float cosine, float refractiveIndex) {
	float r0 = (1 - refractiveIndex) / (1 + refractiveIndex);
	r0 *= r0;
	return r0 + (1 - r0) * pow(1 - cosine, 5);
}

struct Material {
	enum class Type { Lambertian, Metal, Dielectric, DiffuseLight } Type;
	float RefractiveIndex;
	float Roughness;
	float Padding;
	float4 Color;

	bool Emit() {
		switch (Type) {
		case Type::DiffuseLight: return true;
		default: return false;
		}
	}

	bool Scatter(RayDesc ray, HitRecord hitRecord, out float3 direction, inout Random random) {
		switch (Type) {
		case Type::Lambertian: {
			direction = hitRecord.Vertex.Normal + random.UnitVector();

			// Catch degenerate scatter direction
			const float O = 1e-8;
			if (abs(direction[0]) < O && abs(direction[1]) < O && abs(direction[2]) < O) direction = hitRecord.Vertex.Normal;

			return true;
		}

		case Type::Metal: {
			const float3 unitDirection = normalize(ray.Direction);
			direction = reflect(unitDirection, hitRecord.Vertex.Normal) + Roughness * random.InUnitSphere();

			return dot(direction, hitRecord.Vertex.Normal) > 0;
		}

		case Type::Dielectric: {
			const float3 unitDirection = normalize(ray.Direction);
			const float cosTheta = dot(-unitDirection, hitRecord.Vertex.Normal), sinTheta = sqrt(1 - cosTheta * cosTheta),
				refractionRatio = hitRecord.IsFrontFace ? 1 / RefractiveIndex : RefractiveIndex;
			const bool canRefract = refractionRatio * sinTheta <= 1;
			direction = canRefract && SchlickReflectance(cosTheta, refractionRatio) < random.Float3() ?
				refract(unitDirection, hitRecord.Vertex.Normal, refractionRatio) :
				reflect(unitDirection, hitRecord.Vertex.Normal);

			return true;
		}

		default: return false;
		}
	}
};
#endif
