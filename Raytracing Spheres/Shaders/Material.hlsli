#pragma once

#include "HitInfo.hlsli"

#include "Random.hlsli"

inline float SchlickReflectance(float cosine, float refractiveIndex) {
	float r0 = (1 - refractiveIndex) / (1 + refractiveIndex);
	r0 *= r0;
	return r0 + (1 - r0) * pow(1 - cosine, 5);
}

struct Material {
	enum class Type { Lambertian, Metal, Dielectric, Isotropic, DiffuseLight } Type;
	float RefractiveIndex, Roughness;
	float _padding;
	float4 Color;

	bool IsEmissive() {
		switch (Type) {
		case Type::DiffuseLight: return true;
		default: return false;
		}
	}

	bool Scatter(float3 rayDirection, HitInfo hitInfo, out float3 direction, inout Random random) {
		switch (Type) {
		case Type::Lambertian: {
			direction = hitInfo.Vertex.Normal + random.UnitVector();

			// Catch degenerate scatter direction
			const float O = 1e-8;
			if (abs(direction[0]) < O && abs(direction[1]) < O && abs(direction[2]) < O) direction = hitInfo.Vertex.Normal;
		} break;

		case Type::Metal: {
			direction = reflect(rayDirection, hitInfo.Vertex.Normal) + Roughness * random.InUnitSphere();

			return dot(direction, hitInfo.Vertex.Normal) > 0;
		}

		case Type::Dielectric: {
			const float
				cosTheta = dot(-rayDirection, hitInfo.Vertex.Normal), sinTheta = sqrt(1 - cosTheta * cosTheta),
				refractionRatio = hitInfo.IsFrontFace ? 1 / RefractiveIndex : RefractiveIndex;
			const bool canRefract = refractionRatio * sinTheta <= 1;
			direction = canRefract && SchlickReflectance(cosTheta, refractionRatio) <= random.Float() ?
				refract(rayDirection, hitInfo.Vertex.Normal, refractionRatio) :
				reflect(rayDirection, hitInfo.Vertex.Normal);
		} break;

		case Type::Isotropic: direction = random.InUnitSphere(); break;

		default: return false;
		}

		return true;
	}
};
