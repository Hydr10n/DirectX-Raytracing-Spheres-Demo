#pragma once

#include "Material.hlsli"

#include "SurfaceVectors.hlsli"

enum class LobeType
{
	DiffuseReflection, SpecularReflection, SpecularTransmission
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

struct BSDFSample
{
	float3 BaseColor;
	float Metallic;
	float3 Albedo;
	float Roughness;
	float3 Rf0;
	float Transmission, IOR;

	void Initialize(float3 baseColor, float metallic, float roughness, float transmission = 0, float IOR = 1)
	{
		BaseColor = baseColor;
		Metallic = metallic;
		BRDF::ConvertBaseColorMetalnessToAlbedoRf0(baseColor.rgb, metallic, Albedo, Rf0);
		Roughness = max(MinRoughness, roughness);
		Transmission = transmission;
		this.IOR = IOR;
	}

	void Initialize(Material material)
	{
		Initialize(material.BaseColor.rgb, material.Metallic, material.Roughness, material.Transmission, material.IOR);
	}

	bool SampleDiffuseReflection(SurfaceVectors surfaceVectors, float3 V, float2 random, out float3 L)
	{
		const float3x3 basis = surfaceVectors.ShadingBasis;
		L = Geometry::RotateVectorInverse(basis, ImportanceSampling::Cosine::GetRay(random));
		return dot(surfaceVectors.FrontNormal, L) > 0;
	}

	float EvaluateDiffuseReflectionPDF(SurfaceVectors surfaceVectors, float3 L)
	{
		if (dot(surfaceVectors.FrontNormal, L) > 0)
		{
			const float3 N = surfaceVectors.ShadingNormal;
			const float NoL = abs(dot(N, L));
			return ImportanceSampling::Cosine::GetPDF(NoL);
		}
		return 0;
	}

	float3 EvaluateDiffuseReflection(SurfaceVectors surfaceVectors, float3 L, float3 V)
	{
		if (dot(surfaceVectors.FrontNormal, L) > 0)
		{
			const float3 N = surfaceVectors.ShadingNormal, H = normalize(L + V);
			const float
				NoL = abs(dot(N, L)),
				NoV = abs(dot(N, V)),
				VoH = abs(dot(V, H)),
				transmissionProbability = Transmission * (1 - Metallic);
			return NoL * Albedo * BRDF::DiffuseTerm(Roughness, NoL, NoV, VoH) * (1 - transmissionProbability);
		}
		return 0;
	}

	bool SampleSpecularReflection(SurfaceVectors surfaceVectors, float3 V, float2 random, out float3 L)
	{
		const float3x3 basis = surfaceVectors.ShadingBasis;
		const float3
			Vlocal = Geometry::RotateVector(basis, V),
			H = Geometry::RotateVectorInverse(basis, ImportanceSampling::VNDF::GetRay(random, Roughness, Vlocal));
		L = reflect(-V, H);
		return dot(surfaceVectors.FrontNormal, L) > 0;
	}

	float EvaluateSpecularReflectionPDF(SurfaceVectors surfaceVectors, float3 L, float3 V)
	{
		if (dot(surfaceVectors.FrontNormal, L) > 0)
		{
			const float3 N = surfaceVectors.ShadingNormal, H = normalize(L + V);
			const float3x3 basis = surfaceVectors.ShadingBasis;
			const float3 Vlocal = Geometry::RotateVector(basis, V);
			const float NoH = abs(dot(N, H));
			return ImportanceSampling::VNDF::GetPDF(Vlocal, NoH, Roughness);
		}
		return 0;
	}

	float3 EvaluateSpecularReflection(SurfaceVectors surfaceVectors, float3 L, float3 V)
	{
		if (dot(surfaceVectors.FrontNormal, L) > 0)
		{
			const float3 N = surfaceVectors.ShadingNormal, H = normalize(L + V);
			const float
				NoL = abs(dot(N, L)),
				NoV = abs(dot(N, V)),
				NoH = abs(dot(N, H)),
				VoH = abs(dot(V, H));
			const float
				D = BRDF::DistributionTerm(Roughness, NoH),
				Gmod = BRDF::GeometryTermMod(Roughness, NoL, NoV, VoH, NoH),
				transmissionProbability = Transmission * (1 - Metallic);
			const float3 F = BRDF::FresnelTerm(Rf0, VoH);
			return NoL * D * Gmod * F * (1 - transmissionProbability);
		}
		return 0;
	}

	bool SampleSpecularTransmission(SurfaceVectors surfaceVectors, float3 V, float3 random, out float3 L)
	{
		const float3x3 basis = surfaceVectors.ShadingBasis;
		const float3
			Vlocal = Geometry::RotateVector(basis, V),
			H = Geometry::RotateVectorInverse(basis, ImportanceSampling::VNDF::GetRay(random.xy, Roughness, Vlocal));
		const float VoH = abs(dot(V, H)), eta = surfaceVectors.IsFront ? BRDF::IOR::Vacuum / IOR : IOR;
		if (eta * eta * (1 - VoH * VoH) > 1
			|| random.z < BRDF::FresnelTerm_Schlick(pow((IOR - 1) / (IOR + 1), 2), VoH).x)
		{
			L = reflect(-V, H);
		}
		else
		{
			L = refract(-V, H, eta);
			L = any(isnan(L)) ? -V : L;
		}
		return true;
	}

	float EvaluateSpecularTransmissionPDF(SurfaceVectors surfaceVectors, float3 L, float3 V)
	{
		const float3 N = surfaceVectors.ShadingNormal;
		const float NoL = abs(dot(N, L));
		return NoL;
	}

	float3 EvaluateSpecularTransmission(SurfaceVectors surfaceVectors, float3 L, float3 V)
	{
		const float3 N = surfaceVectors.ShadingNormal;
		const float NoL = abs(dot(N, L));
		const float transmissionProbability = Transmission * (1 - Metallic);
		return NoL * BaseColor * transmissionProbability;
	}

	bool Sample(SurfaceVectors surfaceVectors, float3 V, float4 random, out float3 L, out LobeType lobeType, out float lobeProbability)
	{
		const float transmissionProbability = Transmission * (1 - Metallic);
		if (random.x < transmissionProbability)
		{
			lobeType = LobeType::SpecularTransmission;
			lobeProbability = transmissionProbability;
			return SampleSpecularTransmission(surfaceVectors, V, random.yzw, L);
		}
		const float3 N = surfaceVectors.ShadingNormal;
		const float
			NoV = abs(dot(N, V)),
			diffuseProbability = EstimateDiffuseProbability(Albedo, Rf0, Roughness, NoV),
			reflectionProbability = 1 - transmissionProbability;
		if (random.y < diffuseProbability)
		{
			lobeType = LobeType::DiffuseReflection;
			lobeProbability = reflectionProbability * diffuseProbability;
			return SampleDiffuseReflection(surfaceVectors, V, random.zw, L);
		}
		lobeType = LobeType::SpecularReflection;
		lobeProbability = reflectionProbability * (1 - diffuseProbability);
		return SampleSpecularReflection(surfaceVectors, V, random.zw, L);
	}

	float EvaluatePDF(SurfaceVectors surfaceVectors, float3 L, float3 V)
	{
		const float transmissionProbability = Transmission * (1 - Metallic);
		float BSDF = 0;
		if (transmissionProbability > 0)
		{
			BSDF = EvaluateSpecularTransmissionPDF(surfaceVectors, L, V);
		}
		float BRDF = 0;
		if (transmissionProbability < 1 && dot(surfaceVectors.FrontNormal, L) > 0)
		{
			const float3 N = surfaceVectors.ShadingNormal;
			const float NoV = abs(dot(N, V));
			BRDF = lerp(
				EvaluateSpecularReflectionPDF(surfaceVectors, L, V),
				EvaluateDiffuseReflectionPDF(surfaceVectors, L),
				EstimateDiffuseProbability(Albedo, Rf0, Roughness, NoV)
			);
		}
		return lerp(BRDF, BSDF, transmissionProbability);
	}

	void Evaluate(SurfaceVectors surfaceVectors, float3 L, float3 V, out float3 diffuse, out float3 specular)
	{
		diffuse = 0;
		specular = 0;
		const float transmissionProbability = Transmission * (1 - Metallic);
		if (transmissionProbability > 0)
		{
			specular = EvaluateSpecularTransmission(surfaceVectors, L, V) * transmissionProbability;
		}
		float3 BRDF = 0;
		if (transmissionProbability < 1 && dot(surfaceVectors.FrontNormal, L) > 0)
		{
			const float3 N = surfaceVectors.ShadingNormal;
			const float
				NoV = abs(dot(N, V)),
				diffuseProbability = EstimateDiffuseProbability(Albedo, Rf0, Roughness, NoV),
				reflectionProbability = 1 - transmissionProbability;
			diffuse = EvaluateDiffuseReflection(surfaceVectors, L, V) * reflectionProbability * diffuseProbability;
			specular += EvaluateSpecularReflection(surfaceVectors, L, V) * reflectionProbability * (1 - diffuseProbability);
		}
	}

	float EvaluatePDF(SurfaceVectors surfaceVectors, float3 L, float3 V, LobeType lobeType)
	{
		switch (lobeType)
		{
			case LobeType::DiffuseReflection: return EvaluateDiffuseReflectionPDF(surfaceVectors, L);
			case LobeType::SpecularReflection: return EvaluateSpecularReflectionPDF(surfaceVectors, L, V);
			case LobeType::SpecularTransmission: return EvaluateSpecularTransmissionPDF(surfaceVectors, L, V);
			default: return 0;
		}
	}

	float3 Evaluate(SurfaceVectors surfaceVectors, float3 L, float3 V, LobeType lobeType)
	{
		switch (lobeType)
		{
			case LobeType::DiffuseReflection: return EvaluateDiffuseReflection(surfaceVectors, L, V);
			case LobeType::SpecularReflection: return EvaluateSpecularReflection(surfaceVectors, L, V);
			case LobeType::SpecularTransmission: return EvaluateSpecularTransmission(surfaceVectors, L, V);
			default: return 0;
		}
	}
};
