#pragma once

#include "STL.hlsli"

#include "HitInfo.hlsli"

enum class ScatterType { DiffuseReflection, SpecularReflection, SpecularTransmission };

struct ScatterResult {
	ScatterType Type;
	float3 Direction, Attenuation;
};

struct Material {
	float4 BaseColor, EmissiveColor;
	float Metallic, Roughness, Opacity, RefractiveIndex;
	float AmbientOcclusion;
	float3 _;

	float EstimateDiffuseProbability(float3 N, float3 V, float3 albedo, float3 Rf0) {
		const float3 Fenvironment = STL::BRDF::EnvironmentTerm_Ross(Rf0, abs(dot(N, V)), Roughness);
		const float
			diffuse = STL::Color::Luminance(albedo * (1 - Fenvironment)), specular = STL::Color::Luminance(Fenvironment),
			diffProb = diffuse / (diffuse + specular + 1e-6);
		return diffProb < 5e-3 ? 0 : diffProb;
	}

	ScatterResult Scatter(HitInfo hitInfo, float3 worldRayDirection) {
		/*
		 * Sampling the GGX Distribution of Visible Normals
		 * https://jcgt.org/published/0007/04/01/
		 */

		ScatterResult scatterResult;

		const float2 random = STL::Rng::GetFloat2();

		const float3 N = hitInfo.Vertex.Normal, V = -worldRayDirection;
		const float3x3 basis = STL::Geometry::GetBasis(N);

		float3 H, L;
		float VoH;

		if (BaseColor.a != 1 || Opacity != 1) {
			H = STL::Geometry::RotateVectorInverse(basis, STL::ImportanceSampling::VNDF::GetRay(random, Roughness, STL::Geometry::RotateVector(basis, V)));

			VoH = abs(dot(V, H));

			const float refractiveIndex = hitInfo.IsFrontFace ? STL::BRDF::IOR::Vacuum / RefractiveIndex : RefractiveIndex;
			if (refractiveIndex * sqrt(1 - VoH * VoH) > 1 || STL::Rng::GetFloat2().x < STL::BRDF::FresnelTerm_Dielectric(refractiveIndex, VoH)) {
				L = reflect(-V, H);

				scatterResult.Type = ScatterType::SpecularReflection;
			}
			else {
				L = refract(-V, H, refractiveIndex);

				scatterResult.Type = ScatterType::SpecularTransmission;
			}

			scatterResult.Direction = L;
			scatterResult.Attenuation = BaseColor.rgb * (1 - (Opacity == 1 ? BaseColor.a : Opacity)) * AmbientOcclusion;

			return scatterResult;
		}

		float3 albedo, Rf0;
		STL::BRDF::ConvertBaseColorMetalnessToAlbedoRf0(BaseColor.rgb, Metallic, albedo, Rf0);

		const float diffuseProbability = EstimateDiffuseProbability(N, V, albedo, Rf0);

		if (STL::Rng::GetFloat2().x < diffuseProbability) {
			L = STL::Geometry::RotateVectorInverse(basis, STL::ImportanceSampling::Cosine::GetRay(random));
			H = normalize(V + L);

			scatterResult.Type = ScatterType::DiffuseReflection;
			scatterResult.Attenuation = 1;
		}
		else {
			H = STL::Geometry::RotateVectorInverse(basis, STL::ImportanceSampling::VNDF::GetRay(random, Roughness, STL::Geometry::RotateVector(basis, V)));
			L = reflect(-V, H);

			scatterResult.Type = ScatterType::SpecularReflection;
			scatterResult.Attenuation = STL::BRDF::GeometryTerm_Smith(Roughness, abs(dot(N, L)));
		}

		scatterResult.Direction = L;

		if (dot(hitInfo.VertexUnmappedNormal, L) <= 0) scatterResult.Attenuation = 0;
		else {
			VoH = abs(dot(V, H));

			const float3 F = STL::BRDF::FresnelTerm_Schlick(Rf0, VoH);
			scatterResult.Attenuation *= scatterResult.Type == ScatterType::DiffuseReflection ? albedo * (1 - F) / diffuseProbability : F / (1 - diffuseProbability);
		}

		return scatterResult;
	}
};
