#pragma once

#define NRD_HEADER_ONLY
#include "NRDEncoding.hlsli"
#include "NRD.hlsli"

#include "BxDF.hlsli"

enum class NRDDenoiser
{
	None, ReBLUR, ReLAX
};

struct NRDSettings
{
	NRDDenoiser Denoiser;
	uint3 _;
	float4 HitDistanceParameters;
};

void PackNoisySignals(
	NRDSettings settings,
	float3 N, float3 V, float linearDepth,
	BRDFSample BRDFSample,
	float3 directDiffuse, float3 directSpecular, float lightDistance,
	float4 indirectDiffuseHitDistance, float4 indirectSpecularHitDistance, bool isIndirectPacked,
	out float4 diffuseHitDistance, out float4 specularHitDistance
)
{
	if (settings.Denoiser == NRDDenoiser::ReBLUR)
	{
		if (isIndirectPacked)
		{
			indirectDiffuseHitDistance = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(indirectDiffuseHitDistance);
			indirectSpecularHitDistance = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(indirectSpecularHitDistance);
			diffuseHitDistance.a = REBLUR_GetHitDist(indirectDiffuseHitDistance.a, linearDepth, settings.HitDistanceParameters, 1);
			specularHitDistance.a = indirectSpecularHitDistance.a;
		}
		else
		{
			diffuseHitDistance.a = indirectDiffuseHitDistance.a;
			specularHitDistance.a = REBLUR_FrontEnd_GetNormHitDist(indirectSpecularHitDistance.a, linearDepth, settings.HitDistanceParameters, BRDFSample.Roughness);
		}
	}
	else if (settings.Denoiser == NRDDenoiser::ReLAX)
	{
		if (isIndirectPacked)
		{
			indirectDiffuseHitDistance = RELAX_BackEnd_UnpackRadiance(indirectDiffuseHitDistance);
			indirectSpecularHitDistance = RELAX_BackEnd_UnpackRadiance(indirectSpecularHitDistance);
		}
		diffuseHitDistance.a = indirectDiffuseHitDistance.a;
		specularHitDistance.a = indirectSpecularHitDistance.a;
	}
	const float3 indirectDiffuse = indirectDiffuseHitDistance.rgb, indirectSpecular = indirectSpecularHitDistance.rgb;
	float3 diffuseFactor, specularFactor;
	NRD_MaterialFactors(N, V, BRDFSample.Albedo, BRDFSample.Rf0, BRDFSample.Roughness, diffuseFactor, specularFactor);
	diffuseFactor = 1 / diffuseFactor;
	specularFactor = 1 / specularFactor;
	const float3
		diffuse = directDiffuse * diffuseFactor + indirectDiffuse * (isIndirectPacked ? 1 : diffuseFactor),
		specular = directSpecular * specularFactor + indirectSpecular * (isIndirectPacked ? 1 : specularFactor);
	const float
		directLuminance = Color::Luminance(directDiffuse),
		indirectLuminance = Color::Luminance(indirectDiffuse),
		directHitDistanceContribution = min(directLuminance / (directLuminance + indirectLuminance + 1e-3f), 0.5f);
	diffuseHitDistance.a = lerp(diffuseHitDistance.a, lightDistance, directHitDistanceContribution);
	if (settings.Denoiser == NRDDenoiser::ReBLUR)
	{
		diffuseHitDistance.a = REBLUR_FrontEnd_GetNormHitDist(diffuseHitDistance.a, linearDepth, settings.HitDistanceParameters, 1);
		diffuseHitDistance = REBLUR_FrontEnd_PackRadianceAndNormHitDist(diffuse, diffuseHitDistance.a, true);
		specularHitDistance = REBLUR_FrontEnd_PackRadianceAndNormHitDist(specular, specularHitDistance.a, true);
	}
	else if (settings.Denoiser == NRDDenoiser::ReLAX)
	{
		diffuseHitDistance = RELAX_FrontEnd_PackRadianceAndHitDist(diffuse, diffuseHitDistance.a, true);
		specularHitDistance = RELAX_FrontEnd_PackRadianceAndHitDist(specular, specularHitDistance.a, true);
	}
}

void UnpackDenoisedSignals(
	NRDDenoiser denoiser,
	float3 N, float3 V,
	BRDFSample BRDFSample,
	inout float4 diffuseHitDistance, inout float4 specularHitDistance
)
{
	if (denoiser == NRDDenoiser::ReBLUR)
	{
		diffuseHitDistance = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(diffuseHitDistance);
		specularHitDistance = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(specularHitDistance);
	}
	else if (denoiser == NRDDenoiser::ReLAX)
	{
		diffuseHitDistance = RELAX_BackEnd_UnpackRadiance(diffuseHitDistance);
		specularHitDistance = RELAX_BackEnd_UnpackRadiance(specularHitDistance);
	}
	float3 diffuseFactor, specularFactor;
	NRD_MaterialFactors(N, V, BRDFSample.Albedo, BRDFSample.Rf0, BRDFSample.Roughness, diffuseFactor, specularFactor);
	diffuseHitDistance.rgb *= diffuseFactor;
	specularHitDistance.rgb *= specularFactor;
}
