/*
 * Microfacet Models for Refraction through Rough Surfaces
 * https://www.cs.cornell.edu/~srm/publications/EGSR07-btdf.pdf
 *
 * Real Shading in Unreal Engine 4
 * https://de45xmedrsdbp.cloudfront.net/Resources/files/2013SiggraphPresentationsNotes-26915738.pdf
 *
 * DirectX Raytracing, Tutorial 14: Swapping out a Lambertian BRDF for a GGX BRDF model
 * https://cwyman.org/code/dxrTutors/tutors/Tutor14/tutorial14.md.html
 */

#pragma once

#include "Math.hlsli"

namespace BxDFs {
	inline float SchlickFresnelF0(float refractiveIndex) {
		const float f = (1 - refractiveIndex) / (1 + refractiveIndex);
		return f * f;
	}

	inline float SchlickFresnel(float cosine, float refractiveIndex) { return lerp(pow(1 - cosine, 5), 1, SchlickFresnelF0(refractiveIndex)); }

	inline float3 SchlickFresnel(float cosine, float3 f0) { return lerp(pow(1 - cosine, 5), 1, f0); }

	struct GGX {
		static float NormalDistribution(float NoH, float roughness) {
			const float a = roughness * roughness, d = NoH * NoH * (a - 1) + 1;
			return a / max(1e-6f, Numbers::Pi * d * d);
		}

		static float SchlickGeometry(float cosine, float k) { return cosine / (cosine * (1 - k) + k); }

		static float SmithGeometryDirect(float NoL, float NoV, float roughness) {
			const float r = roughness + 1, k = r * r / 8;
			return SchlickGeometry(NoL, k) * SchlickGeometry(NoV, k);
		}

		static float SmithGeometryIndirect(float NoL, float NoV, float roughness) {
			const float k = roughness * roughness / 2;
			return SchlickGeometry(NoL, k) * SchlickGeometry(NoV, k);
		}

		// Sample according to normal distribution. PDF = D * NoH / (4 * HoL)
		static float3 ImportanceSample(float3 N, float roughness, inout Random random) {
			const float2 value = random.Float2();
			const float3 B = Math::CalculatePerpendicularVector(N), T = cross(B, N);
			const float
				a = roughness * roughness,
				cosTheta = sqrt((1 - value.y) / (1 + (a * a - 1) * value.y)), sinTheta = sqrt(1 - cosTheta * cosTheta),
				phi = 2 * Numbers::Pi * value.x;
			return T * (sinTheta * cos(phi)) + B * (sinTheta * sin(phi)) + N * cosTheta;
		}
	};
}
