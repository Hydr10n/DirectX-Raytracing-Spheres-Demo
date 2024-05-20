#pragma once

#define NRD_HEADER_ONLY
#include "NRDEncoding.hlsli"
#include "NRD.hlsli"

#include "STL.hlsli"

enum class NRDDenoiser { None, ReBLUR, ReLAX };

struct NRDSettings {
	NRDDenoiser Denoiser;
	uint3 _;
	float4 HitDistanceParameters;
};

void PackNoisySignals(
	NRDSettings settings,
	float NoV, float linearDepth,
	float3 albedo, float3 Rf0, float roughness,
	float3 directDiffuse, float3 directSpecular, float lightDistance,
	float4 indirectDiffuseHitDistance, float4 indirectSpecularHitDistance, bool isIndirectDenoised,
	out float4 diffuseHitDistance, out float4 specularHitDistance
) {
	if (settings.Denoiser == NRDDenoiser::ReBLUR) {
		if (isIndirectDenoised) {
			indirectDiffuseHitDistance = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(indirectDiffuseHitDistance);
			indirectSpecularHitDistance = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(indirectSpecularHitDistance);
			diffuseHitDistance.a = REBLUR_GetHitDist(indirectDiffuseHitDistance.a, linearDepth, settings.HitDistanceParameters, 1);
			specularHitDistance.a = indirectSpecularHitDistance.a;
		}
		else {
			diffuseHitDistance.a = indirectDiffuseHitDistance.a;
			specularHitDistance.a = REBLUR_FrontEnd_GetNormHitDist(indirectSpecularHitDistance.a, linearDepth, settings.HitDistanceParameters, roughness);
		}
	}
	else if (settings.Denoiser == NRDDenoiser::ReLAX) {
		if (isIndirectDenoised) {
			indirectDiffuseHitDistance = RELAX_BackEnd_UnpackRadiance(indirectDiffuseHitDistance);
			indirectSpecularHitDistance = RELAX_BackEnd_UnpackRadiance(indirectSpecularHitDistance);
		}
		diffuseHitDistance.a = indirectDiffuseHitDistance.a;
		specularHitDistance.a = indirectSpecularHitDistance.a;
	}
	const float3
		indirectDiffuse = indirectDiffuseHitDistance.rgb, indirectSpecular = indirectSpecularHitDistance.rgb,
		Fenvironment = STL::BRDF::EnvironmentTerm_Rtg(Rf0, NoV, roughness),
		diffuseDemodulation = 1 / lerp((1 - Fenvironment) * albedo, 1, 0.01f),
		specularDemodulation = 1 / lerp(Fenvironment, 1, 0.01f),
		diffuse = directDiffuse * diffuseDemodulation + indirectDiffuse * (isIndirectDenoised ? 1 : diffuseDemodulation),
		specular = directSpecular * specularDemodulation + indirectSpecular * (isIndirectDenoised ? 1 : specularDemodulation);
	const float
		directLuminance = STL::Color::Luminance(directDiffuse),
		indirectLuminance = STL::Color::Luminance(indirectDiffuse),
		directHitDistanceContribution = min(directLuminance / (directLuminance + indirectLuminance + 1e-3f), 0.5f);
	diffuseHitDistance.a = lerp(diffuseHitDistance.a, lightDistance, directHitDistanceContribution);
	if (settings.Denoiser == NRDDenoiser::ReBLUR) {
		diffuseHitDistance.a = REBLUR_FrontEnd_GetNormHitDist(diffuseHitDistance.a, linearDepth, settings.HitDistanceParameters, 1);
		diffuseHitDistance = REBLUR_FrontEnd_PackRadianceAndNormHitDist(diffuse, diffuseHitDistance.a, true);
		specularHitDistance = REBLUR_FrontEnd_PackRadianceAndNormHitDist(specular, specularHitDistance.a, true);
	}
	else if (settings.Denoiser == NRDDenoiser::ReLAX) {
		diffuseHitDistance = RELAX_FrontEnd_PackRadianceAndHitDist(diffuse, diffuseHitDistance.a, true);
		specularHitDistance = RELAX_FrontEnd_PackRadianceAndHitDist(specular, specularHitDistance.a, true);
	}
}

void UnpackDenoisedSignals(
	NRDDenoiser denoiser,
	float NoV,
	float3 albedo, float3 Rf0, float roughness,
	inout float4 diffuseHitDistance, inout float4 specularHitDistance
) {
	if (denoiser == NRDDenoiser::ReBLUR) {
		diffuseHitDistance = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(diffuseHitDistance);
		specularHitDistance = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(specularHitDistance);
	}
	else if (denoiser == NRDDenoiser::ReLAX) {
		diffuseHitDistance = RELAX_BackEnd_UnpackRadiance(diffuseHitDistance);
		specularHitDistance = RELAX_BackEnd_UnpackRadiance(specularHitDistance);
	}
	const float3 Fenvironment = STL::BRDF::EnvironmentTerm_Rtg(Rf0, NoV, roughness);
	diffuseHitDistance.rgb *= lerp((1 - Fenvironment) * albedo, 1, 0.01f);
	specularHitDistance.rgb *= lerp(Fenvironment, 1, 0.01f);
}
