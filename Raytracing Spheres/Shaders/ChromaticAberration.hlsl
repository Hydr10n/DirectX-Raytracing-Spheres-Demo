#include "Math.hlsli"

SamplerState g_linearSampler : register(s0);

Texture2D<float3> g_input : register(t0);

RWTexture2D<float3> g_output : register(u0);

cbuffer _ : register(b0) { uint2 g_renderSize; }

cbuffer Data : register(b1) {
	float2 g_focusUV;
	float2 _;
	float3 g_offsets;
	float _1;
}

#define ROOT_SIGNATURE \
	"StaticSampler(s0, filter=FILTER_MIN_MAG_MIP_LINEAR, addressU=TEXTURE_ADDRESS_CLAMP, addressV=TEXTURE_ADDRESS_CLAMP)," \
	"DescriptorTable(SRV(t0))," \
	"DescriptorTable(UAV(u0))," \
	"RootConstants(num32BitConstants=2, b0)," \
	"CBV(b1)"

[RootSignature(ROOT_SIGNATURE)]
[numthreads(16, 16, 1)]
void main(uint2 pixelCoordinate : SV_DispatchThreadID) {
	if (pixelCoordinate.x >= g_renderSize.x || pixelCoordinate.y >= g_renderSize.y) return;

	const float2 UV = Math::CalculateUV(pixelCoordinate, g_renderSize), direction = UV - g_focusUV;
	float3 color;
	color.r = g_input.SampleLevel(g_linearSampler, UV + direction * g_offsets.r, 0).r;
	color.g = g_input.SampleLevel(g_linearSampler, UV + direction * g_offsets.g, 0).g;
	color.b = g_input.SampleLevel(g_linearSampler, UV + direction * g_offsets.b, 0).b;
	g_output[pixelCoordinate] = color;
}
