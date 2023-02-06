#pragma once

#include "BxDFs.hlsli"

#include "HitInfo.hlsli"

struct ScatterResult {
	float3 Direction;
	float4 Attenuation;
};

struct Material {
	float4 BaseColor, EmissiveColor;
	float3 Specular; // Amount of dielectric specular reflection. Specifies facing (along normal) reflectivity in the most common 0 - 8% range.
	float Metallic, Roughness, Opacity, RefractiveIndex;
	float AmbientOcclusion;

	ScatterResult Scatter(HitInfo hitInfo, float3 worldRayDirection, inout Random random) {
		ScatterResult scatterResult;

		const float3 N = hitInfo.Vertex.Normal, V = -worldRayDirection;
		float3 L;

		float3 f0;
		if (BaseColor.a == 1 && Opacity == 1) f0 = Specular * 0.08f;
		else {
			const float3 H = BxDFs::GGX::ImportanceSample(N, Roughness, random);

			const float VoH = saturate(dot(V, H)), refractiveIndex = hitInfo.IsFrontFace ? 1 / RefractiveIndex : RefractiveIndex;

			L = refractiveIndex * sqrt(1 - VoH * VoH) > 1 || random.Float() < BxDFs::SchlickFresnel(VoH, refractiveIndex) ? reflect(worldRayDirection, H) : refract(worldRayDirection, H, refractiveIndex);

			scatterResult.Direction = L;
			scatterResult.Attenuation = BaseColor * (1 - (Opacity == 1 ? BaseColor.a : Opacity)) * AmbientOcclusion;

			return scatterResult;
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

			scatterResult.Direction = L;
			scatterResult.Attenuation = dot(hitInfo.VertexUnmappedNormal, L) <= 0 ? 0 : BaseColor / diffuseProbability * AmbientOcclusion;
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

			scatterResult.Direction = L;
			if (dot(hitInfo.VertexUnmappedNormal, L) <= 0) scatterResult.Attenuation = 0;
			else {
				const float
					NoL = saturate(dot(N, L)), NoV = saturate(dot(N, V)), NoH = saturate(dot(N, H)), HoL = saturate(dot(H, L)),
					G = BxDFs::GGX::SmithGeometryIndirect(NoL, NoV, Roughness);
				const float3 F = BxDFs::SchlickFresnel(HoL, lerp(f0, BaseColor.rgb, Metallic));
				scatterResult.Attenuation = float4(G * F * HoL / (NoV * NoH * (1 - diffuseProbability)), 1) * AmbientOcclusion;
			}
		}

		return scatterResult;
	}
};
