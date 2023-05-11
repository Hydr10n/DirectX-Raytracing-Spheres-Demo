#pragma once

#include "STL.hlsli"

#include "HitInfo.hlsli"

enum class ScatterType { DiffuseReflection, SpecularReflection, SpecularTransmission };

struct ScatterResult {
	ScatterType Type;
	float3 Direction, Attenuation;
};

enum class AlphaMode { Opaque, Blend, Mask };

struct Material {
	float4 BaseColor, EmissiveColor;
	float Metallic, Roughness, Opacity, RefractiveIndex;
	AlphaMode AlphaMode;
	float AlphaThreshold, AmbientOcclusion;

	float EstimateDiffuseProbability(float3 N, float3 V, float3 albedo, float3 Rf0) {
		const float3 Fenvironment = STL::BRDF::EnvironmentTerm_Ross(Rf0, abs(dot(N, V)), Roughness);
		const float
			diffuse = STL::Color::Luminance(albedo * (1 - Fenvironment)), specular = STL::Color::Luminance(Fenvironment),
			diffuseProbability = diffuse / (diffuse + specular + 1e-6f);
		return diffuseProbability < 5e-3f ? 0 : diffuseProbability;
	}

	ScatterResult Scatter(HitInfo hitInfo, float3 worldRayDirection) {
		/*
		 * Sampling the GGX Distribution of Visible Normals
		 * https://jcgt.org/published/0007/04/01/
		 */

		ScatterResult scatterResult;
		scatterResult.Attenuation = AmbientOcclusion;

		float3 albedo, Rf0;
		STL::BRDF::ConvertBaseColorMetalnessToAlbedoRf0(BaseColor.rgb, Metallic, albedo, Rf0);

		const float2 random = STL::Rng::Hash::GetFloat2();

		const float3 N = hitInfo.Vertex.Normal, V = -worldRayDirection;
		const float3x3 basis = STL::Geometry::GetBasis(N);

		float3 H, L;

		if (Opacity != 1) {
			H = STL::Geometry::RotateVectorInverse(basis, STL::ImportanceSampling::VNDF::GetRay(random, Roughness, STL::Geometry::RotateVector(basis, V)));

			const float VoH = abs(dot(V, H));
			const float refractiveIndex = hitInfo.IsFrontFace ? STL::BRDF::IOR::Vacuum / RefractiveIndex : RefractiveIndex;
			if (refractiveIndex * sqrt(1 - VoH * VoH) > 1 || STL::Rng::Hash::GetFloat() < STL::BRDF::FresnelTerm_Dielectric(refractiveIndex, VoH)) {
				L = reflect(-V, H);

				scatterResult.Type = ScatterType::SpecularReflection;
			}
			else {
				L = refract(-V, H, refractiveIndex);

				scatterResult.Type = ScatterType::SpecularTransmission;
			}

			scatterResult.Direction = L;
			scatterResult.Attenuation *= albedo * (1 - Opacity);

			return scatterResult;
		}

		const float diffuseProbability = EstimateDiffuseProbability(N, V, albedo, Rf0);

		if (STL::Rng::Hash::GetFloat() < diffuseProbability) {
			L = STL::Geometry::RotateVectorInverse(basis, STL::ImportanceSampling::Cosine::GetRay(random));
			H = normalize(V + L);

			scatterResult.Type = ScatterType::DiffuseReflection;
			scatterResult.Attenuation *= albedo / diffuseProbability;
		}
		else {
			H = STL::Geometry::RotateVectorInverse(basis, STL::ImportanceSampling::VNDF::GetRay(random, Roughness, STL::Geometry::RotateVector(basis, V)));
			L = reflect(-V, H);

			scatterResult.Type = ScatterType::SpecularReflection;
			scatterResult.Attenuation *= STL::BRDF::GeometryTerm_Smith(Roughness, abs(dot(N, L))) / (1 - diffuseProbability);
		}

		scatterResult.Direction = L;

		if (dot(hitInfo.UnmappedVertexNormal, L) < 0) scatterResult.Attenuation = 0;
		else {
			const float3 F = STL::BRDF::FresnelTerm_Schlick(Rf0, abs(dot(V, H)));
			scatterResult.Attenuation *= scatterResult.Type == ScatterType::DiffuseReflection ? 1 - F : F;
		}

		return scatterResult;
	}
};
