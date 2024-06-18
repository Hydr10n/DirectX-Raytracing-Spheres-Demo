#pragma once

#include "STL.hlsli"

#include "HitInfo.hlsli"

enum class ScatterType
{
	DiffuseReflection, SpecularReflection, Transmission
};

struct ScatterResult
{
	ScatterType Type;
	float3 Direction, Throughput;
};

enum class AlphaMode
{
	Opaque, Blend, Mask
};

static const float MinRoughness = 5e-2f;

struct Material
{
	float4 BaseColor;
	float3 EmissiveColor;
	float Metallic, Roughness, Transmission, RefractiveIndex;
	AlphaMode AlphaMode;
	float AlphaThreshold;
	float3 _;

	static float EstimateDiffuseProbability(float3 albedo, float3 Rf0, float roughness, float NoV)
	{
		const float3 Fenvironment = STL::BRDF::EnvironmentTerm_Ross(Rf0, NoV, roughness);
		const float
			diffuse = STL::Color::Luminance(albedo * (1 - Fenvironment)), specular = STL::Color::Luminance(Fenvironment),
			diffuseProbability = diffuse / (diffuse + specular + 1e-6f);
		return diffuseProbability < 5e-3f ? 0 : diffuseProbability;
	}

	ScatterResult Scatter(HitInfo hitInfo, float3 worldRayDirection, float minRoughness = MinRoughness)
	{
		ScatterResult scatterResult;
		scatterResult.Throughput = 1;

		float3 albedo, Rf0;
		STL::BRDF::ConvertBaseColorMetalnessToAlbedoRf0(BaseColor.rgb, Metallic, albedo, Rf0);
		
		const float3 N = hitInfo.Normal, V = -worldRayDirection;
		const float NoV = abs(dot(N, V)), roughness = max(Roughness, MinRoughness);
		const float diffuseProbability = EstimateDiffuseProbability(albedo, Rf0, roughness, NoV);
		if (STL::Rng::Hash::GetFloat() <= diffuseProbability)
		{
			const float transmissiveProbability = diffuseProbability * Transmission;
			if (STL::Rng::Hash::GetFloat() <= Transmission)
			{
				scatterResult.Type = ScatterType::Transmission;
				scatterResult.Throughput /= transmissiveProbability;
			}
			else
			{
				scatterResult.Type = ScatterType::DiffuseReflection;
				scatterResult.Throughput /= diffuseProbability - transmissiveProbability;
			}
		}
		else
		{
			scatterResult.Type = ScatterType::SpecularReflection;
			scatterResult.Throughput /= 1 - diffuseProbability;
		}

		float3 L;
		const float3x3 basis = STL::Geometry::GetBasis(N);
		const float2 randomValue = STL::Rng::Hash::GetFloat2();
		if (scatterResult.Type == ScatterType::DiffuseReflection)
		{
			L = STL::Geometry::RotateVectorInverse(basis, STL::ImportanceSampling::Cosine::GetRay(randomValue));
			const float3 H = normalize(V + L);
			const float NoL = abs(dot(N, L)), VoH = abs(dot(V, H));

			scatterResult.Throughput *= albedo * STL::Math::Pi(1) * STL::BRDF::DiffuseTerm_Burley(roughness, NoL, NoV, VoH);
		}
		else
		{
			const float3 H = STL::Geometry::RotateVectorInverse(basis, STL::ImportanceSampling::VNDF::GetRay(randomValue, roughness, STL::Geometry::RotateVector(basis, V)));
			const float VoH = abs(dot(V, H));
			if (scatterResult.Type == ScatterType::SpecularReflection)
			{
				L = reflect(-V, H);
				const float NoL = abs(dot(N, L)), VoH = abs(dot(V, H));
				
				scatterResult.Throughput *= STL::BRDF::FresnelTerm_Schlick(Rf0, VoH) * STL::BRDF::GeometryTerm_Smith(roughness, NoL);
			}
			else
			{
				const float refractiveIndex = hitInfo.IsFrontFace ? STL::BRDF::IOR::Vacuum / RefractiveIndex : RefractiveIndex;
				if (refractiveIndex * sqrt(1 - VoH * VoH) > 1 || STL::Rng::Hash::GetFloat() <= STL::BRDF::FresnelTerm_Dielectric(refractiveIndex, VoH))
				{
					L = reflect(-V, H);
				}
				else
				{
					L = refract(-V, H, refractiveIndex);
				}
				
				scatterResult.Throughput *= albedo;
			}
		}
		
		scatterResult.Direction = L;

		if (scatterResult.Type != ScatterType::Transmission && dot(hitInfo.GeometricNormal, L) <= 0)
		{
			scatterResult.Throughput = 0;
		}

		return scatterResult;
	}
};
