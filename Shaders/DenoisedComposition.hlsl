#define NRD_HEADER_ONLY
#include "NRDEncoding.hlsli"
#include "NRD.hlsli"
#include "NRDDenoiser.hlsli"

#include "Math.hlsli"

Texture2D<float> g_linearDepth : register(t0);
Texture2D<float4> g_baseColorMetalness : register(t1);
Texture2D<float3> g_emissiveColor : register(t2);
Texture2D<float4> g_normalRoughness : register(t3);
Texture2D<float4> g_denoisedDiffuse : register(t4);
Texture2D<float4> g_denoisedSpecular : register(t5);

RWTexture2D<float3> g_color : register(u0);

cbuffer _ : register(b0) { uint2 g_renderSize; }

cbuffer Data : register(b1) {
	NRDDenoiser g_NRDDenoiser;
	float3 g_cameraRightDirection;
	float3 g_cameraUpDirection;
	float _;
	float3 g_cameraForwardDirection;
	float _1;
	float2 g_cameraPixelJitter;
	float2 _2;
}

#define ROOT_SIGNATURE \
	"DescriptorTable(SRV(t0))," \
	"DescriptorTable(SRV(t1))," \
	"DescriptorTable(SRV(t2))," \
	"DescriptorTable(SRV(t3))," \
	"DescriptorTable(SRV(t4))," \
	"DescriptorTable(SRV(t5))," \
	"DescriptorTable(UAV(u0))," \
	"RootConstants(num32BitConstants=2, b0)," \
	"CBV(b1)"

[RootSignature(ROOT_SIGNATURE)]
[numthreads(16, 16, 1)]
void main(uint2 pixelCoordinate : SV_DispatchThreadID) {
	if (pixelCoordinate.x >= g_renderSize.x || pixelCoordinate.y >= g_renderSize.y) return;

	if (g_linearDepth[pixelCoordinate] == 1.#INFf) return;

	float3 albedo, Rf0;
	const float4 baseColorMetalness = g_baseColorMetalness[pixelCoordinate];
	STL::BRDF::ConvertBaseColorMetalnessToAlbedoRf0(baseColorMetalness.rgb, baseColorMetalness.a, albedo, Rf0);

	const float2 NDC = Math::CalculateNDC(Math::CalculateUV(pixelCoordinate, g_renderSize, g_cameraPixelJitter));
	const float4 normalRoughness = NRD_FrontEnd_UnpackNormalAndRoughness(g_normalRoughness[pixelCoordinate]);
	float3 diffuse = 0, specular = 0;
	const float3
		V = -normalize(NDC.x * g_cameraRightDirection + NDC.y * g_cameraUpDirection + g_cameraForwardDirection),
		Fenvironment = STL::BRDF::EnvironmentTerm_Rtg(Rf0, abs(dot(normalRoughness.xyz, V)), normalRoughness.w);
	if (g_NRDDenoiser == NRDDenoiser::ReBLUR) {
		diffuse = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(g_denoisedDiffuse[pixelCoordinate]).rgb;
		specular = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(g_denoisedSpecular[pixelCoordinate]).rgb;
	}
	else if (g_NRDDenoiser == NRDDenoiser::ReLAX) {
		diffuse = RELAX_BackEnd_UnpackRadiance(g_denoisedDiffuse[pixelCoordinate]).rgb;
		specular = RELAX_BackEnd_UnpackRadiance(g_denoisedSpecular[pixelCoordinate]).rgb;
	}
	diffuse *= lerp((1 - Fenvironment) * albedo, 1, 0.01f);
	specular *= lerp(Fenvironment, 1, 0.01f);
	g_color[pixelCoordinate] = diffuse + specular + g_emissiveColor[pixelCoordinate];
}
