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
	enum class Type { Lambertian, Metal, Dielectric, Isotropic, DiffuseLight } Type;
	float RefractiveIndex, Roughness, Padding;
	float4 Color;

	bool IsEmissive() {
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
			direction = reflect(ray.Direction, hitRecord.Vertex.Normal) + Roughness * random.InUnitSphere();

			return dot(direction, hitRecord.Vertex.Normal) > 0;
		}

		case Type::Isotropic: {
			direction = random.InUnitSphere();

			return true;
		}

		case Type::Dielectric: {
			const float cosTheta = dot(-ray.Direction, hitRecord.Vertex.Normal), sinTheta = sqrt(1 - cosTheta * cosTheta),
				refractionRatio = hitRecord.IsFrontFace ? 1 / RefractiveIndex : RefractiveIndex;
			const bool canRefract = refractionRatio * sinTheta <= 1;
			direction = canRefract && SchlickReflectance(cosTheta, refractionRatio) < random.Float3() ?
				refract(ray.Direction, hitRecord.Vertex.Normal, refractionRatio) :
				reflect(ray.Direction, hitRecord.Vertex.Normal);

			return true;
		}

		default: return false;
		}
	}
};

#endif
