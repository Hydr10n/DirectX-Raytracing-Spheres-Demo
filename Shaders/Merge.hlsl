#include "Math.hlsli"

SamplerState g_linearSampler : register(s0);

struct Constants { float Weight1, Weight2; };
ConstantBuffer<Constants> g_constants : register(b0);

Texture2D<float3> g_input1 : register(t0);
Texture2D<float3> g_input2 : register(t1);

RWTexture2D<float3> g_output : register(u0);

[RootSignature(
	"StaticSampler(s0, filter=FILTER_MIN_MAG_MIP_LINEAR, addressU=TEXTURE_ADDRESS_CLAMP, addressV=TEXTURE_ADDRESS_CLAMP),"
	"RootConstants(num32BitConstants=2, b0),"
	"DescriptorTable(SRV(t0)),"
	"DescriptorTable(SRV(t1)),"
	"DescriptorTable(UAV(u0))"
)]
[numthreads(16, 16, 1)]
void main(uint2 pixelPosition : SV_DispatchThreadID) {
	float2 pixelDimensions;
	g_output.GetDimensions(pixelDimensions.x, pixelDimensions.y);
	if (any(pixelPosition >= pixelDimensions)) return;

	const float2 UV = Math::CalculateUV(pixelPosition, pixelDimensions);
	g_output[pixelPosition] = g_input1.SampleLevel(g_linearSampler, UV, 0) * g_constants.Weight1 + g_input2.SampleLevel(g_linearSampler, UV, 0) * g_constants.Weight2;
}
