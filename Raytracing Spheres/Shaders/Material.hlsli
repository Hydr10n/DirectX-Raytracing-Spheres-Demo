#pragma once

#include "HitInfo.hlsli"

#include "Math.hlsli"

#include "Random.hlsli"

struct Material {
	enum class Type { Lambertian, Metal, Dielectric, Isotropic, DiffuseLight } Type;
	float RefractiveIndex, Roughness;
	float _padding;
	float4 BaseColor;

	bool IsEmissive() {
		switch (Type) {
		case Type::DiffuseLight: return true;
		default: return false;
		}
	}

	bool Scatter(float3 rayDirection, HitInfo hitInfo, out float3 direction, out float4 attenuation, inout Random random) {
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
				refractiveIndex = hitInfo.IsFrontFace ? 1 / RefractiveIndex : RefractiveIndex;
			const bool canRefract = refractiveIndex * sinTheta <= 1;
			direction = canRefract && Math::SchlickFresnel(cosTheta, refractiveIndex) <= random.Float() ?
				refract(rayDirection, hitInfo.Vertex.Normal, refractiveIndex) :
				reflect(rayDirection, hitInfo.Vertex.Normal);
		} break;

		case Type::Isotropic: direction = random.InUnitSphere(); break;

		default: return false;
		}

		attenuation = BaseColor;

		return true;
	}
};
