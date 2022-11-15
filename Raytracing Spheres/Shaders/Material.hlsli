#pragma once

#include "HitInfo.hlsli"

#include "BxDFs.hlsli"

struct Material {
	float4 BaseColor, EmissiveColor;
	float3 Specular; // Amount of dielectric specular reflection. Specifies facing (along normal) reflectivity in the most common 0 - 8% range.
	float Metallic, Roughness, Opacity, RefractiveIndex;
	float AmbientOcclusion;

	void Scatter(HitInfo hitInfo, float3 worldRayDirection, out float3 L, out float4 attenuation, inout Random random) {
		const float3 N = hitInfo.Vertex.Normal;

		float3 f0;
		if (BaseColor.a >= 0.94f && Opacity == 1) f0 = Specular * 0.08f;
		else {
			const float3 H = BxDFs::GGX::ImportanceSample(N, Roughness, random);

			const float
				cosTheta = saturate(dot(-worldRayDirection, H)),
				sinTheta = sqrt(1 - cosTheta * cosTheta);

			float refractiveIndex = RefractiveIndex == 0 ? 1 : RefractiveIndex;
			refractiveIndex = hitInfo.IsFrontFace ? 1 / refractiveIndex : refractiveIndex;

			L = refractiveIndex * sinTheta > 1 || random.Float() < BxDFs::SchlickFresnel(cosTheta, refractiveIndex) ? reflect(worldRayDirection, H) : refract(worldRayDirection, H, refractiveIndex);

			attenuation = BaseColor * (1 - (Opacity == 1 ? BaseColor.a : Opacity)) * AmbientOcclusion;

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

			L = Math::SampleCosineHemisphere(N, random);

			attenuation = dot(hitInfo.VertexUnmappedNormal, L) <= 0 ? 0 : BaseColor / diffuseProbability * AmbientOcclusion;
		}
		else {
			/*
			 * IncidentLight = ReflectedLight * NoL
			 * BRDF = D * G * F / (4 * NoL * NoV)
			 * PDF = D * NoH / (4 * HoL)
			 * ReflectedLight *= G * F * HoL / (NoV * NoH)
			 */

			const float3 H = BxDFs::GGX::ImportanceSample(N, Roughness, random);

			L = reflect(worldRayDirection, H);

			if (dot(hitInfo.VertexUnmappedNormal, L) <= 0) attenuation = 0;
			else {
				const float
					NoL = saturate(dot(N, L)), NoV = saturate(dot(N, -worldRayDirection)), NoH = saturate(dot(N, H)), HoL = saturate(dot(H, L)),
					G = BxDFs::GGX::SmithGeometryIndirect(NoL, NoV, Roughness);
				const float3 F = BxDFs::SchlickFresnel(HoL, lerp(f0, BaseColor.rgb, Metallic));
				attenuation = float4(G * F * HoL / (NoV * NoH * (1 - diffuseProbability)), 1) * AmbientOcclusion;
			}
		}
	}
};
