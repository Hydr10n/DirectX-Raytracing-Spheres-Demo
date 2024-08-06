#pragma once

#include "Material.hlsli"

#include "HitInfo.hlsli"

enum class LobeType
{
	DiffuseReflection, SpecularReflection, Transmission
};

static const float MinRoughness = 5e-2f;

static float EstimateDiffuseProbability(float3 albedo, float3 Rf0, float roughness, float NoV)
{
	const float3 Fenvironment = BRDF::EnvironmentTerm_Ross(Rf0, NoV, roughness);
	const float
		diffuse = Color::Luminance(albedo * (1 - Fenvironment)), specular = Color::Luminance(Fenvironment),
		diffuseProbability = diffuse / (diffuse + specular + 1e-6f);
	return diffuseProbability < 5e-3f ? 0 : diffuseProbability;
}

struct BRDFSample
{
	float3 Albedo, Rf0;
	float Roughness;

	void Initialize(float3 baseColor, float metallic, float roughness)
	{
		BRDF::ConvertBaseColorMetalnessToAlbedoRf0(baseColor.rgb, metallic, Albedo, Rf0);
		Roughness = roughness;
	}

	void Initialize(Material material)
	{
		Initialize(material.BaseColor.rgb, material.Metallic, material.Roughness);
	}

	bool Sample(HitInfo hitInfo, float3 V, out LobeType lobeType, out float3 L, out float PDF, out float weight, float minRoughness = MinRoughness)
	{
		const float3 N = hitInfo.Normal;
		const float NoV = abs(dot(N, V)), roughness = max(Roughness, MinRoughness);
		const float diffuseProbability = EstimateDiffuseProbability(Albedo, Rf0, roughness, NoV);
		if (Rng::Hash::GetFloat() <= diffuseProbability)
		{
			lobeType = LobeType::DiffuseReflection;
			weight = 1 / diffuseProbability;
		}
		else
		{
			lobeType = LobeType::SpecularReflection;
			weight = 1 / (1 - diffuseProbability);
		}

		float NoL, NoH;
		const float3x3 basis = Geometry::GetBasis(N);
		const float3 Vlocal = Geometry::RotateVector(basis, V);
		const float2 randomValue = Rng::Hash::GetFloat2();
		if (lobeType == LobeType::DiffuseReflection)
		{
			L = Geometry::RotateVectorInverse(basis, ImportanceSampling::Cosine::GetRay(randomValue));
			NoL = abs(dot(N, L));
			const float3 H = normalize(V + L);
			NoH = abs(dot(N, H));
		}
		else
		{
			const float3 H = Geometry::RotateVectorInverse(basis, ImportanceSampling::VNDF::GetRay(randomValue, roughness, Vlocal));
			NoH = abs(dot(N, H));
			L = reflect(-V, H);
			NoL = abs(dot(N, L));
		}

		const bool ret = dot(hitInfo.GeometricNormal, L) > 0;
		if (ret)
		{
			PDF = lerp(ImportanceSampling::VNDF::GetPDF(Vlocal, NoH, roughness), ImportanceSampling::Cosine::GetPDF(NoL), diffuseProbability);
		}
		else
		{
			PDF = 0;
			weight = 0;
		}
		return ret;
	}

	float3 Evaluate(LobeType lobeType, float3 N, float3 V, float3 L, float minRoughness = MinRoughness)
	{
		const float3 H = normalize(V + L);
		const float NoL = abs(dot(N, L)), VoH = abs(dot(V, H)), roughness = max(Roughness, minRoughness);
		if (lobeType == LobeType::SpecularReflection)
		{
			return BRDF::FresnelTerm_Schlick(Rf0, VoH) * BRDF::GeometryTerm_Smith(roughness, NoL);
		}
		const float NoV = abs(dot(N, V));
		return Albedo * Math::Pi(1) * BRDF::DiffuseTerm_Burley(roughness, NoL, NoV, VoH);
	}
};

struct BSDFSample : BRDFSample
{
	float Transmission, IOR;

	void Initialize(float3 baseColor, float metallic, float roughness, float transmission, float IOR)
	{
		BRDFSample::Initialize(baseColor, metallic, roughness);
		Transmission = transmission;
		this.IOR = IOR;
	}

	void Initialize(Material material)
	{
		BRDFSample::Initialize(material);
		Transmission = material.Transmission;
		IOR = material.IOR;
	}

	bool Sample(HitInfo hitInfo, float3 V, out LobeType lobeType, out float3 L, out float PDF, out float weight, float minRoughness = MinRoughness)
	{
		const float3 N = hitInfo.Normal;
		const float
			NoV = abs(dot(N, V)),
			roughness = max(Roughness, MinRoughness),
			diffuseProbability = EstimateDiffuseProbability(Albedo, Rf0, roughness, NoV);
		if (Rng::Hash::GetFloat() <= diffuseProbability)
		{
			const float transmissiveProbability = diffuseProbability * Transmission;
			if (Rng::Hash::GetFloat() <= Transmission)
			{
				lobeType = LobeType::Transmission;
				weight = 1 / transmissiveProbability;
			}
			else
			{
				lobeType = LobeType::DiffuseReflection;
				weight = 1 / (diffuseProbability - transmissiveProbability);
			}
		}
		else
		{
			lobeType = LobeType::SpecularReflection;
			weight = 1 / (1 - diffuseProbability);
		}

		float NoL, NoH;
		const float3x3 basis = Geometry::GetBasis(N);
		const float3 Vlocal = Geometry::RotateVector(basis, V);
		const float2 randomValue = Rng::Hash::GetFloat2();
		if (lobeType == LobeType::DiffuseReflection)
		{
			L = Geometry::RotateVectorInverse(basis, ImportanceSampling::Cosine::GetRay(randomValue));
			NoL = abs(dot(N, L));
			const float3 H = normalize(V + L);
			NoH = abs(dot(N, H));
		}
		else
		{
			const float3 H = Geometry::RotateVectorInverse(basis, ImportanceSampling::VNDF::GetRay(randomValue, roughness, Vlocal));
			NoH = abs(dot(N, H));
			if (lobeType == LobeType::SpecularReflection)
			{
				L = reflect(-V, H);
				NoL = abs(dot(N, L));
			}
			else
			{
				const float VoH = abs(dot(V, H)), eta = hitInfo.IsFrontFace ? BRDF::IOR::Vacuum / IOR : IOR;
				if (eta * sqrt(1 - VoH * VoH) > 1 || Rng::Hash::GetFloat() <= BRDF::FresnelTerm_Dielectric(eta, VoH))
				{
					L = reflect(-V, H);
				}
				else
				{
					L = refract(-V, H, eta);
				}
				NoL = abs(dot(N, L));
			}
		}

		const bool ret = lobeType == LobeType::Transmission || dot(hitInfo.GeometricNormal, L) > 0;
		if (ret)
		{
			PDF = lerp(ImportanceSampling::VNDF::GetPDF(Vlocal, NoH, roughness), ImportanceSampling::Cosine::GetPDF(NoL), diffuseProbability);
		}
		else
		{
			PDF = 0;
			weight = 0;
		}
		return ret;
	}

	float3 Evaluate(LobeType lobeType, float3 N, float3 V, float3 L, float minRoughness = MinRoughness)
	{
		if (lobeType == LobeType::Transmission)
		{
			return Albedo;
		}
		return BRDFSample::Evaluate(lobeType, N, V, L, minRoughness);
	}
};
