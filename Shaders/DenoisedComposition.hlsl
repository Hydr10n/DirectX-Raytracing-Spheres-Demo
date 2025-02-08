#include "Denoiser.hlsli"

#include "Camera.hlsli"

#include "Math.hlsli"

struct Constants
{
	uint2 RenderSize;
	Denoiser Denoiser;
};
ConstantBuffer<Constants> g_constants : register(b0);

ConstantBuffer<Camera> g_camera : register(b1);

Texture2D<float> g_linearDepth : register(t0);
Texture2D<float4> g_baseColorMetalness : register(t1);
Texture2D<float4> g_normalRoughness : register(t2);
Texture2D<float4> g_denoisedDiffuse : register(t3);
Texture2D<float4> g_denoisedSpecular : register(t4);

RWTexture2D<float3> g_radiance : register(u0);

[RootSignature(
	"RootConstants(num32BitConstants=3, b0),"
	"CBV(b1),"
	"DescriptorTable(SRV(t0)),"
	"DescriptorTable(SRV(t1)),"
	"DescriptorTable(SRV(t2)),"
	"DescriptorTable(SRV(t3)),"
	"DescriptorTable(SRV(t4)),"
	"DescriptorTable(UAV(u0))"
)]
[numthreads(16, 16, 1)]
void main(uint2 pixelPosition : SV_DispatchThreadID)
{
	if (any(pixelPosition >= g_constants.RenderSize) || isinf(g_linearDepth[pixelPosition]))
	{
		return;
	}

	const float4 baseColorMetalness = g_baseColorMetalness[pixelPosition], normalRoughness = g_normalRoughness[pixelPosition];

	BSDFSample BSDFSample;
	BSDFSample.Initialize(baseColorMetalness.rgb, baseColorMetalness.a, normalRoughness.w);

	const float2 NDC = Math::CalculateNDC(Math::CalculateUV(pixelPosition, g_constants.RenderSize, g_camera.Jitter));
	const float3 V = -normalize(g_camera.GenerateRayDirection(NDC));
	float4 diffuseHitDistance = g_denoisedDiffuse[pixelPosition], specularHitDistance = g_denoisedSpecular[pixelPosition];
	NRDUnpackDenoisedSignals(
		g_constants.Denoiser,
		normalRoughness.xyz, V,
		BSDFSample,
		diffuseHitDistance, specularHitDistance
	);
	g_radiance[pixelPosition] += diffuseHitDistance.rgb + specularHitDistance.rgb;
}
