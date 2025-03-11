#pragma once

#include "Material.hlsli"

#include "SurfaceVectors.hlsli"

#include "Denoiser.hlsli"

enum class LobeType
{
	DiffuseReflection,
	SpecularReflection,
	SpecularTransmission,
	Count
};

using LobeWeightArray = float[(uint)LobeType::Count];

static const float MinRoughness = 2e-3f;

float EstimateDiffuseProbability(float3 albedo, float3 f0, float roughness, float NoV)
{
	const float3 Fenvironment = BRDF::EnvironmentTerm_Rtg(f0, NoV, roughness);
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
	float Roughness, IORi, IORo;
	float3 F0;
	float Transmission;

	void Initialize(
		float3 baseColor,
		float metallic,
		float roughness,
		float IOR,
		float transmission,
		bool isFrontFace
	)
	{
		BaseColor = baseColor;
		Metallic = metallic;
		Albedo = baseColor * (1 - metallic);
		Roughness = max(MinRoughness, roughness);
		IORi = 1;
		IORo = IOR;
		if (!isFrontFace)
		{
			IORi = IOR;
			IORo = 1;
		}
		F0 = lerp(pow((IORi - IORo) / (IORi + IORo), 2), baseColor, metallic);
		Transmission = transmission;
	}

	void Initialize(Material material, bool isFrontFace)
	{
		Initialize(
			material.BaseColor.rgb,
			material.Metallic,
			material.Roughness,
			material.IOR,
			material.Transmission,
			isFrontFace
		);
	}

	bool SampleDiffuseReflection(SurfaceVectors surfaceVectors, float3 V, float2 random, out float3 L)
	{
		const float3x3 basis = surfaceVectors.ShadingBasis;
		L = Geometry::RotateVectorInverse(basis, ImportanceSampling::Cosine::GetRay(random));
		return dot(surfaceVectors.FrontGeometricNormal, L) > 0;
	}

	float EvaluateDiffuseReflectionPDF(SurfaceVectors surfaceVectors, float3 L)
	{
		if (dot(surfaceVectors.FrontGeometricNormal, L) > 0)
		{
			const float3 N = surfaceVectors.ShadingNormal;
			const float NoL = abs(dot(N, L));
			return ImportanceSampling::Cosine::GetPDF(NoL);
		}
		return 0;
	}

	float3 EvaluateDiffuseReflection(SurfaceVectors surfaceVectors, float3 L, float3 V, float3 H)
	{
		if (dot(surfaceVectors.FrontGeometricNormal, L) > 0)
		{
			const float3 N = surfaceVectors.ShadingNormal;
			const float NoL = abs(dot(N, L)), NoV = abs(dot(N, V)), VoH = abs(dot(V, H));
			return NoL * Albedo * BRDF::DiffuseTerm(Roughness, NoL, NoV, VoH);
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
		return dot(surfaceVectors.FrontGeometricNormal, L) > 0;
	}

	float EvaluateSpecularReflectionPDF(SurfaceVectors surfaceVectors, float3 L, float3 V, float3 H)
	{
		if (dot(surfaceVectors.FrontGeometricNormal, L) > 0)
		{
			const float3 N = surfaceVectors.ShadingNormal;
			const float3x3 basis = surfaceVectors.ShadingBasis;
			const float3 Vlocal = Geometry::RotateVector(basis, V);
			const float NoH = abs(dot(N, H));
			return ImportanceSampling::VNDF::GetPDF(Vlocal, NoH, Roughness);
		}
		return 0;
	}

	float3 EvaluateSpecularReflection(SurfaceVectors surfaceVectors, float3 L, float3 V, float3 H)
	{
		if (dot(surfaceVectors.FrontGeometricNormal, L) > 0)
		{
			const float3 N = surfaceVectors.ShadingNormal;
			const float
				NoL = abs(dot(N, L)), NoV = abs(dot(N, V)), VoH = abs(dot(V, H)), NoH = abs(dot(N, H)),
				D = BRDF::DistributionTerm(Roughness, NoH),
				Gmod = BRDF::GeometryTermMod(Roughness, NoL, NoV, VoH, NoH);
			const float3 F = BRDF::FresnelTerm(F0, VoH);
			return NoL * D * Gmod * F;
		}
		return 0;
	}

	bool SampleSpecularTransmission(SurfaceVectors surfaceVectors, float3 V, float3 random, out float3 L)
	{
		const float3x3 basis = surfaceVectors.ShadingBasis;
		const float3
			Vlocal = Geometry::RotateVector(basis, V),
			H = Geometry::RotateVectorInverse(basis, ImportanceSampling::VNDF::GetRay(random.xy, Roughness, Vlocal));
		const float VoH = abs(dot(V, H)), eta = IORi / IORo;
		if (eta * eta * (1 - VoH * VoH) > 1 || random.z < BRDF::FresnelTerm_Dielectric(eta, VoH))
		{
			L = reflect(-V, H);
		}
		else
		{
			L = refract(-V, H, eta);
			if (any(!isfinite(L)))
			{
				L = -V;
			}
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
		return NoL * BaseColor;
	}

	void ComputeLobeWeights(SurfaceVectors surfaceVectors, float3 V, out LobeWeightArray lobeWeights)
	{
		const float3 N = surfaceVectors.ShadingNormal;
		const float NoV = abs(dot(N, V));
		const float
			transmissionWeight = Transmission * (1 - Metallic),
			reflectionWeight = 1 - transmissionWeight,
			diffuseWeight = EstimateDiffuseProbability(Albedo, F0, Roughness, NoV),
			specularWeight = 1 - diffuseWeight;
		lobeWeights[(uint)LobeType::DiffuseReflection] = diffuseWeight * reflectionWeight;
		lobeWeights[(uint)LobeType::SpecularReflection] = specularWeight * reflectionWeight;
		lobeWeights[(uint)LobeType::SpecularTransmission] = transmissionWeight;
	}

	LobeType FindLobe(LobeWeightArray lobeWeights, float random)
	{
		uint lobe = (uint)LobeType::Count;
		float weight = 0;
		[[unroll]]
		while (--lobe > 0)
		{
			weight += lobeWeights[lobe];
			if (random < weight)
			{
				break;
			}
		}
		return (LobeType)lobe;
	}

	bool Sample(
		SurfaceVectors surfaceVectors, float3 V, LobeWeightArray lobeWeights, float4 random,
		out float3 L, out LobeType lobeType
	)
	{
		switch (lobeType = FindLobe(lobeWeights, random.x))
		{
			case LobeType::DiffuseReflection: return SampleDiffuseReflection(surfaceVectors, V, random.yz, L);
			case LobeType::SpecularReflection: return SampleSpecularReflection(surfaceVectors, V, random.yz, L);
			case LobeType::SpecularTransmission: return SampleSpecularTransmission(surfaceVectors, V, random.yzw, L);
			default: return false;
		}
	}

	float3 ComputeHalfVector(SurfaceVectors surfaceVectors, float3 L, float3 V, bool isTransmissve)
	{
		const float3 N = surfaceVectors.FrontGeometricNormal;
		float3 H;
		if (isTransmissve && dot(N, L) < 0)
		{
			H = normalize(L * IORo + V * IORi);
			if (dot(N, H) < 0)
			{
				H = -H;
			}
		}
		else
		{
			H = normalize(L + V);
		}
		return H;
	}

	float EvaluatePDF(SurfaceVectors surfaceVectors, float3 L, float3 V, LobeWeightArray lobeWeights)
	{
		float PDF = 0;
		const float transmissionWeight = lobeWeights[(uint)LobeType::SpecularTransmission];
		const float3 H = ComputeHalfVector(surfaceVectors, L, V, transmissionWeight > 0);
		if (transmissionWeight > 0)
		{
			PDF = EvaluateSpecularTransmissionPDF(surfaceVectors, L, V) * transmissionWeight;
		}
		if (transmissionWeight < 1 && dot(surfaceVectors.FrontGeometricNormal, L) > 0)
		{
			const float3 N = surfaceVectors.ShadingNormal;
			const float NoV = abs(dot(N, V));
			PDF += EvaluateDiffuseReflectionPDF(surfaceVectors, L) * lobeWeights[(uint)LobeType::DiffuseReflection]
				+ EvaluateSpecularReflectionPDF(surfaceVectors, L, V, H) * lobeWeights[(uint)LobeType::SpecularReflection];
		}
		return PDF;
	}

	void Evaluate(
		SurfaceVectors surfaceVectors, float3 L, float3 V, LobeWeightArray lobeWeights,
		out float3 diffuse, out float3 specular
	)
	{
		diffuse = 0;
		specular = 0;
		const float transmissionWeight = lobeWeights[(uint)LobeType::SpecularTransmission];
		const float3 H = ComputeHalfVector(surfaceVectors, L, V, transmissionWeight > 0);
		if (transmissionWeight > 0)
		{
			specular = EvaluateSpecularTransmission(surfaceVectors, L, V) * transmissionWeight;
		}
		if (transmissionWeight < 1 && dot(surfaceVectors.FrontGeometricNormal, L) > 0)
		{
			const float reflectionWeight = 1 - transmissionWeight;
			diffuse = EvaluateDiffuseReflection(surfaceVectors, L, V, H) * reflectionWeight;
			specular += EvaluateSpecularReflection(surfaceVectors, L, V, H) * reflectionWeight;
		}
	}

	float EvaluatePDF(SurfaceVectors surfaceVectors, float3 L, float3 V, LobeWeightArray lobeWeights, LobeType lobeType)
	{
		const float transmissionWeight = lobeWeights[(uint)LobeType::SpecularTransmission];
		const float3 H = ComputeHalfVector(surfaceVectors, L, V, transmissionWeight > 0);
		const float lobeWeight = lobeWeights[(uint)lobeType];
		switch (lobeType)
		{
			case LobeType::DiffuseReflection: return EvaluateDiffuseReflectionPDF(surfaceVectors, L) * lobeWeight;
			case LobeType::SpecularReflection: return EvaluateSpecularReflectionPDF(surfaceVectors, L, V, H) * lobeWeight;
			case LobeType::SpecularTransmission: return EvaluateSpecularTransmissionPDF(surfaceVectors, L, V) * lobeWeight;
			default: return 0;
		}
	}

	float3 Evaluate(SurfaceVectors surfaceVectors, float3 L, float3 V, LobeWeightArray lobeWeights, LobeType lobeType)
	{
		const float transmissionWeight = lobeWeights[(uint)LobeType::SpecularTransmission];
		const float3 H = ComputeHalfVector(surfaceVectors, L, V, transmissionWeight > 0);
		if (lobeType == LobeType::SpecularTransmission)
		{
			return EvaluateSpecularTransmission(surfaceVectors, L, V) * transmissionWeight;
		}
		const float reflectionWeight = 1 - transmissionWeight;
		if (lobeType == LobeType::DiffuseReflection)
		{
			return EvaluateDiffuseReflection(surfaceVectors, L, V, H) * reflectionWeight;
		}
		return EvaluateSpecularReflection(surfaceVectors, L, V, H) * reflectionWeight;
	}

	void EstimateDemodulationFactors(SurfaceVectors surfaceVectors, float3 V, out float3 diffuse, out float3 specular)
	{
		NRD_MaterialFactors(surfaceVectors.ShadingNormal, V, Albedo, F0, Roughness, diffuse, specular);
	}
};
