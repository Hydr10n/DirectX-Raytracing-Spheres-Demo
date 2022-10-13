#pragma once

#include "HitInfo.hlsli"

#include "BxDFs.hlsli"

struct Material {
	float4 BaseColor, EmissiveColor;
	float Roughness;
	float3 Specular; // Amount of dielectric specular reflection. Specifies facing (along normal) reflectivity in the most common 0 - 8% range.
	float Metallic, RefractiveIndex;
	float2 _;

	void Scatter(float3 rayDirection, HitInfo hitInfo, out float3 L, out float4 attenuation, inout Random random) {
		const float3 N = hitInfo.Vertex.Normal;

		float3 f0;
		if (Metallic > 0) f0 = 0.04f;
		else if (RefractiveIndex == 0) f0 = Specular * 0.08f;
		else {
			const float3 H = BxDFs::GGX::ImportanceSample(N, Roughness, random);

			const float
				cosTheta = saturate(dot(-rayDirection, H)),
				sinTheta = sqrt(1 - cosTheta * cosTheta),
				refractiveIndex = hitInfo.IsFrontFace ? 1 / RefractiveIndex : RefractiveIndex;

			L = refractiveIndex * sinTheta > 1 || random.Float() < BxDFs::SchlickFresnel(cosTheta, refractiveIndex) ? reflect(rayDirection, H) : refract(rayDirection, H, refractiveIndex);

			attenuation = BaseColor;

			return;
		}

		const float diffuseProbability = (1 - Metallic) / 2;
		if (random.Float() < diffuseProbability) {
			/*
			 * IncidentLight = ReflectedLight * NoL
			 * BRDF = SurfaceColor / Pi
			 * PDF = NoL / Pi
			 * ReflectedLight *= SurfaceColor
			 */

			L = Math::CosineSampleHemisphere(N, random);

			attenuation = BaseColor / diffuseProbability;
		}
		else {
			/*
			 * IncidentLight = ReflectedLight * NoL
			 * BRDF = D * G * F / (4 * NoL * NoV)
			 * PDF = D * NoH / (4 * HoL)
			 * ReflectedLight *= G * F * HoL / (NoV * NoH)
			 */

			const float3 H = BxDFs::GGX::ImportanceSample(N, Roughness, random);

			L = reflect(rayDirection, H);

			const float
				NoL = saturate(dot(N, L)), NoV = saturate(dot(N, -rayDirection)), NoH = saturate(dot(N, H)), HoL = saturate(dot(H, L)),
				G = BxDFs::GGX::SmithGeometryIndirect(NoL, NoV, Roughness);
			const float3 F = BxDFs::SchlickFresnel(HoL, lerp(f0, BaseColor.rgb, Metallic));
			attenuation = float4(G * F * HoL / (NoV * NoH * (1 - diffuseProbability)), 1);
		}
	}
};
