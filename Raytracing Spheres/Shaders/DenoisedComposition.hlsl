#define NRD_HEADER_ONLY
#include "NRDEncoding.hlsli"
#include "NRD.hlsli"

#include "STL.hlsli"

#include "Math.hlsli"

Texture2D<float4> g_normalRoughness : register(t0);
Texture2D<float> g_viewZ : register(t1);
Texture2D<float4> g_baseColorMetalness : register(t2);
Texture2D<float4> g_denoisedDiffuse : register(t3);
Texture2D<float4> g_denoisedSpecular : register(t4);

RWTexture2D<float3> g_output : register(u0);

cbuffer Data : register(b0) {
	float3 g_cameraPosition;
	float _;
	float3 g_cameraRightDirection;
	float _1;
	float3 g_cameraUpDirection;
	float _2;
	float3 g_cameraForwardDirection;
	float _3;
	float2 g_cameraPixelJitter;
	float2 _4;
}

#define ROOT_SIGNATURE \
	"DescriptorTable(SRV(t0))," \
	"DescriptorTable(SRV(t1))," \
	"DescriptorTable(SRV(t2))," \
	"DescriptorTable(SRV(t3))," \
	"DescriptorTable(SRV(t4))," \
	"DescriptorTable(UAV(u0))," \
	"CBV(b0)"

[RootSignature(ROOT_SIGNATURE)]
[numthreads(16, 16, 1)]
void main(uint2 pixelCoordinate : SV_DispatchThreadID) {
	uint2 pixelDimensions;
	g_output.GetDimensions(pixelDimensions.x, pixelDimensions.y);
	if (pixelCoordinate.x >= pixelDimensions.x || pixelCoordinate.y >= pixelDimensions.y) return;

	const float viewZ = g_viewZ[pixelCoordinate];
	if (viewZ == 1.#INFf) return;

	float3 albedo, Rf0;
	const float4 baseColorMetalness = g_baseColorMetalness[pixelCoordinate];
	STL::BRDF::ConvertBaseColorMetalnessToAlbedoRf0(baseColorMetalness.rgb, baseColorMetalness.a, albedo, Rf0);

	const float2 NDC = Math::CalculateNDC(Math::CalculateUV(pixelCoordinate, pixelDimensions, g_cameraPixelJitter));
	const float4 normalRoughness = NRD_FrontEnd_UnpackNormalAndRoughness(g_normalRoughness[pixelCoordinate]);
	const float3
		V = -normalize(NDC.x * g_cameraRightDirection + NDC.y * g_cameraUpDirection + g_cameraForwardDirection),
		Fenvironment = STL::BRDF::EnvironmentTerm_Rtg(Rf0, abs(dot(normalRoughness.xyz, V)), normalRoughness.w),
		diffuse = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(g_denoisedDiffuse[pixelCoordinate]).rgb * ((1 - Fenvironment) * albedo * 0.99f + 0.01f),
		specular = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(g_denoisedSpecular[pixelCoordinate]).rgb * (Fenvironment * 0.99f + 0.01f);
	g_output[pixelCoordinate] = diffuse + specular;
}
