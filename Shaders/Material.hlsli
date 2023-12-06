#pragma once

#include "STL.hlsli"

#include "HitInfo.hlsli"

enum class ScatterType { DiffuseReflection, SpecularReflection, SpecularTransmission };

struct ScatterResult {
	ScatterType Type;
	float3 Direction, Throughput;
};

enum class AlphaMode { Opaque, Blend, Mask };

static const float MinRoughness = 1e-3f;

struct Material {
	float4 BaseColor;
	float3 EmissiveColor;
	float Metallic, Roughness, Opacity, RefractiveIndex;
	AlphaMode AlphaMode;
	float AlphaThreshold;

	static float EstimateDiffuseProbability(float3 albedo, float3 Rf0, float roughness, float NoV) {
		const float3 Fenvironment = STL::BRDF::EnvironmentTerm_Ross(Rf0, NoV, roughness);
		const float
			diffuse = STL::Color::Luminance(albedo * (1 - Fenvironment)), specular = STL::Color::Luminance(Fenvironment),
			diffuseProbability = diffuse / (diffuse + specular + 1e-6f);
		return diffuseProbability < 5e-3f ? 0 : diffuseProbability;
	}

	ScatterResult Scatter(HitInfo hitInfo, float3 worldRayDirection, float splitProbability = STL::Rng::Hash::GetFloat()) {
		ScatterResult scatterResult;

		float3 albedo, Rf0;
		STL::BRDF::ConvertBaseColorMetalnessToAlbedoRf0(BaseColor.rgb, Metallic, albedo, Rf0);

		const float2 randomValue = STL::Rng::Hash::GetFloat2();

		const float3 N = hitInfo.Normal, V = -worldRayDirection;
		const float3x3 basis = STL::Geometry::GetBasis(N);

		float3 H, L;

		if (Opacity != 1) {
			H = STL::Geometry::RotateVectorInverse(basis, STL::ImportanceSampling::VNDF::GetRay(randomValue, Roughness, STL::Geometry::RotateVector(basis, V)));

			const float VoH = abs(dot(V, H));
			const float refractiveIndex = hitInfo.IsFrontFace ? STL::BRDF::IOR::Vacuum / RefractiveIndex : RefractiveIndex;
			if (refractiveIndex * sqrt(1 - VoH * VoH) > 1 || splitProbability < STL::BRDF::FresnelTerm_Dielectric(refractiveIndex, VoH)) {
				L = reflect(-V, H);

				scatterResult.Type = ScatterType::SpecularReflection;
			}
			else {
				L = refract(-V, H, refractiveIndex);

				scatterResult.Type = ScatterType::SpecularTransmission;
			}

			scatterResult.Direction = L;
			scatterResult.Throughput = albedo * (1 - Opacity);

			return scatterResult;
		}

		const float NoV = abs(dot(N, V));
		const float diffuseProbability = EstimateDiffuseProbability(albedo, Rf0, Roughness, NoV);
		if (splitProbability < diffuseProbability) {
			L = STL::Geometry::RotateVectorInverse(basis, STL::ImportanceSampling::Cosine::GetRay(randomValue));
			H = normalize(V + L);

			scatterResult.Type = ScatterType::DiffuseReflection;
			scatterResult.Throughput = albedo * STL::Math::Pi(1) * STL::BRDF::DiffuseTerm_Burley(Roughness, abs(dot(N, L)), NoV, abs(dot(V, H))) / diffuseProbability;
		}
		else {
			H = STL::Geometry::RotateVectorInverse(basis, STL::ImportanceSampling::VNDF::GetRay(randomValue, Roughness, STL::Geometry::RotateVector(basis, V)));
			L = reflect(-V, H);

			scatterResult.Type = ScatterType::SpecularReflection;
			scatterResult.Throughput = STL::BRDF::FresnelTerm_Schlick(Rf0, abs(dot(V, H))) * STL::BRDF::GeometryTerm_Smith(Roughness, abs(dot(N, L))) / (1 - diffuseProbability);
		}

		scatterResult.Direction = L;

		if (dot(hitInfo.GeometricNormal, L) <= 0) scatterResult.Throughput = 0;

		return scatterResult;
	}
};
