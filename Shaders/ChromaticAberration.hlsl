#include "Math.hlsli"

SamplerState g_linearSampler : register(s0);

struct Constants
{
	float2 FocusUV;
	float3 Offsets;
};
ConstantBuffer<Constants> g_constants : register(b0);

Texture2D<float3> g_input : register(t0);

RWTexture2D<float3> g_output : register(u0);

[RootSignature(
	"StaticSampler(s0, filter=FILTER_MIN_MAG_MIP_LINEAR, addressU=TEXTURE_ADDRESS_CLAMP, addressV=TEXTURE_ADDRESS_CLAMP),"
	"RootConstants(num32BitConstants=5, b0),"
	"DescriptorTable(SRV(t0)),"
	"DescriptorTable(UAV(u0))"
)]
[numthreads(16, 16, 1)]
void main(uint2 pixelPosition : SV_DispatchThreadID)
{
	float2 pixelDimensions;
	g_output.GetDimensions(pixelDimensions.x, pixelDimensions.y);
	if (any(pixelPosition >= pixelDimensions))
	{
		return;
	}

	const float2 UV = Math::CalculateUV(pixelPosition, pixelDimensions), direction = UV - g_constants.FocusUV;
	g_output[pixelPosition] = float3(
		g_input.SampleLevel(g_linearSampler, UV + direction * g_constants.Offsets.r, 0).r,
		g_input.SampleLevel(g_linearSampler, UV + direction * g_constants.Offsets.g, 0).g,
		g_input.SampleLevel(g_linearSampler, UV + direction * g_constants.Offsets.b, 0).b
	);
}
