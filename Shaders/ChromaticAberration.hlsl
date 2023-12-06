#include "Math.hlsli"

SamplerState g_linearSampler : register(s0);

struct Constants {
	uint2 RenderSize;
	float2 FocusUV;
	float3 Offsets;
};
ConstantBuffer<Constants> g_constants : register(b0);

Texture2D<float3> g_inColor : register(t0);

RWTexture2D<float3> g_outColor : register(u0);

[RootSignature(
	"StaticSampler(s0, filter=FILTER_MIN_MAG_MIP_LINEAR, addressU=TEXTURE_ADDRESS_CLAMP, addressV=TEXTURE_ADDRESS_CLAMP),"
	"RootConstants(num32BitConstants=7, b0),"
	"DescriptorTable(SRV(t0)),"
	"DescriptorTable(UAV(u0))"
)]
[numthreads(16, 16, 1)]
void main(uint2 pixelPosition : SV_DispatchThreadID) {
	if (any(pixelPosition >= g_constants.RenderSize)) return;

	const float2 UV = Math::CalculateUV(pixelPosition, g_constants.RenderSize), direction = UV - g_constants.FocusUV;
	g_outColor[pixelPosition] = float3(
		g_inColor.SampleLevel(g_linearSampler, UV + direction * g_constants.Offsets.r, 0).r,
		g_inColor.SampleLevel(g_linearSampler, UV + direction * g_constants.Offsets.g, 0).g,
		g_inColor.SampleLevel(g_linearSampler, UV + direction * g_constants.Offsets.b, 0).b
		);
}
