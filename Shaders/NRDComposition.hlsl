#include "Denoiser.hlsli"

struct Constants
{
	uint2 RenderSize;
	bool IsPack;
	Denoiser Denoiser;
	float4 ReBLURHitDistance;
};
ConstantBuffer<Constants> g_constants : register(b0);

Texture2D<float> g_linearDepth : register(t0);
Texture2D<float3> g_diffuseAlbedo : register(t1);
Texture2D<float3> g_specularAlbedo : register(t2);
Texture2D<float4> g_normalRoughness : register(t3);
Texture2D<float4> g_denoisedDiffuse : register(t4);
Texture2D<float4> g_denoisedSpecular : register(t5);

RWTexture2D<float4> g_noisyDiffuse : register(u0);
RWTexture2D<float4> g_noisySpecular : register(u1);
RWTexture2D<float3> g_radiance : register(u2);

[RootSignature(
	"RootConstants(num32BitConstants=8, b0),"
	"DescriptorTable(SRV(t0)),"
	"DescriptorTable(SRV(t1)),"
	"DescriptorTable(SRV(t2)),"
	"DescriptorTable(SRV(t3)),"
	"DescriptorTable(SRV(t4)),"
	"DescriptorTable(SRV(t5)),"
	"DescriptorTable(UAV(u0)),"
	"DescriptorTable(UAV(u1)),"
	"DescriptorTable(UAV(u2))"
)]
[numthreads(16, 16, 1)]
void main(uint2 pixelPosition : SV_DispatchThreadID)
{
	if (any(pixelPosition >= g_constants.RenderSize))
	{
		return;
	}

	const float linearDepth = g_linearDepth[pixelPosition];
	if (!isfinite(linearDepth))
	{
		return;
	}

	const float3 diffuseAlbedo = g_diffuseAlbedo[pixelPosition], specularAlbedo = g_specularAlbedo[pixelPosition];
	if (g_constants.IsPack)
	{
		float4 diffuse = g_noisyDiffuse[pixelPosition], specular = g_noisySpecular[pixelPosition];
		diffuse.rgb /= diffuseAlbedo;
		specular.rgb /= specularAlbedo;
		if (g_constants.Denoiser == Denoiser::NRDReBLUR)
		{
			const float roughness = g_normalRoughness[pixelPosition].w;
			diffuse.a = REBLUR_FrontEnd_GetNormHitDist(diffuse.a, linearDepth, g_constants.ReBLURHitDistance, 1);
			specular.a = REBLUR_FrontEnd_GetNormHitDist(specular.a, linearDepth, g_constants.ReBLURHitDistance, roughness);
			diffuse = REBLUR_FrontEnd_PackRadianceAndNormHitDist(diffuse.rgb, diffuse.a, true);
			specular = REBLUR_FrontEnd_PackRadianceAndNormHitDist(specular.rgb, specular.a, true);
		}
		else if (g_constants.Denoiser == Denoiser::NRDReLAX)
		{
			diffuse = RELAX_FrontEnd_PackRadianceAndHitDist(diffuse.rgb, diffuse.a, true);
			specular = RELAX_FrontEnd_PackRadianceAndHitDist(specular.rgb, specular.a, true);
		}
		g_noisyDiffuse[pixelPosition] = diffuse;
		g_noisySpecular[pixelPosition] = specular;
	}
	else
	{
		float4 diffuse = g_denoisedDiffuse[pixelPosition], specular = g_denoisedSpecular[pixelPosition];
		if (g_constants.Denoiser == Denoiser::NRDReBLUR)
		{
			diffuse = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(diffuse);
			specular = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(specular);
		}
		else if (g_constants.Denoiser == Denoiser::NRDReLAX)
		{
			diffuse = RELAX_BackEnd_UnpackRadiance(diffuse);
			specular = RELAX_BackEnd_UnpackRadiance(specular);
		}
		diffuse.rgb *= diffuseAlbedo;
		specular.rgb *= specularAlbedo;
		g_radiance[pixelPosition] += diffuse.rgb + specular.rgb;
	}
}
