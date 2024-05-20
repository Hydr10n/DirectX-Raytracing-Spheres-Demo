#include "Denoiser.hlsli"

#include "Camera.hlsli"

#include "Math.hlsli"

struct Constants {
	uint2 RenderSize;
	NRDDenoiser NRDDenoiser;
};
ConstantBuffer<Constants> g_constants : register(b0);

ConstantBuffer<Camera> g_camera : register(b1);

Texture2D<float> g_linearDepth : register(t0);
Texture2D<float4> g_baseColorMetalness : register(t1);
Texture2D<float3> g_emissiveColor : register(t2);
Texture2D<float4> g_normalRoughness : register(t3);
Texture2D<float4> g_denoisedDiffuse : register(t4);
Texture2D<float4> g_denoisedSpecular : register(t5);

RWTexture2D<float3> g_color : register(u0);

[RootSignature(
	"RootConstants(num32BitConstants=3, b0),"
	"CBV(b1),"
	"DescriptorTable(SRV(t0)),"
	"DescriptorTable(SRV(t1)),"
	"DescriptorTable(SRV(t2)),"
	"DescriptorTable(SRV(t3)),"
	"DescriptorTable(SRV(t4)),"
	"DescriptorTable(SRV(t5)),"
	"DescriptorTable(UAV(u0))"
)]
[numthreads(16, 16, 1)]
void main(uint2 pixelPosition : SV_DispatchThreadID) {
	if (any(pixelPosition >= g_constants.RenderSize)) return;

	if (g_linearDepth[pixelPosition] == 1.#INFf) return;

	float3 albedo, Rf0;
	const float4 baseColorMetalness = g_baseColorMetalness[pixelPosition];
	STL::BRDF::ConvertBaseColorMetalnessToAlbedoRf0(baseColorMetalness.rgb, baseColorMetalness.a, albedo, Rf0);

	const float2 NDC = Math::CalculateNDC(Math::CalculateUV(pixelPosition, g_constants.RenderSize, g_camera.Jitter));
	const float3 V = -normalize(NDC.x * g_camera.RightDirection + NDC.y * g_camera.UpDirection + g_camera.ForwardDirection);
	const float4 normalRoughness = NRD_FrontEnd_UnpackNormalAndRoughness(g_normalRoughness[pixelPosition]);
	float4 diffuseHitDistance = g_denoisedDiffuse[pixelPosition], specularHitDistance = g_denoisedSpecular[pixelPosition];
	UnpackDenoisedSignals(
		g_constants.NRDDenoiser,
		abs(dot(normalRoughness.xyz, V)),
		albedo, Rf0, normalRoughness.w,
		diffuseHitDistance, specularHitDistance
	);
	g_color[pixelPosition] = diffuseHitDistance.rgb + specularHitDistance.rgb + g_emissiveColor[pixelPosition];
}
