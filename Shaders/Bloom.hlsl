/*
 * Physically Based Bloom
 * https://learnopengl.com/Guest-Articles/2022/Phys.-Based-Bloom
 */

#include "Math.hlsli"

SamplerState g_linearSampler : register(s0);

struct Constants
{
	uint InputMipLevel;
	float UpsamplingFilterRadius;
};
ConstantBuffer<Constants> g_constants : register(b0);

Texture2D<float3> g_input : register(t0);

RWTexture2D<float3> g_output : register(u0);

static float2 g_UV, g_size;

float3 Sample(float x, float y)
{
	return g_input.SampleLevel(g_linearSampler, g_UV + g_size * float2(x, y), g_constants.InputMipLevel);
}

float KarisAverage(float3 rgb)
{
	return 1 / (1 + Color::Luminance(Color::ToSrgb(rgb)) * 0.25f);
}

float3 Downsample()
{
	/*
	 * Take 13 samples around current texel 'e':
	 * a - b - c
	 * - j - k -
	 * d - e - f
	 * - l - m -
	 * g - h - i
	 */
	const float3
		a = Sample(-2, 2), b = Sample(0, 2), c = Sample(2, 2),
		d = Sample(-2, 0), e = Sample(0, 0), f = Sample(2, 0),
		g = Sample(-2, -2), h = Sample(0, -2), i = Sample(2, -2),
		j = Sample(-1, 1), k = Sample(1, 1),
		l = Sample(-1, -1), m = Sample(1, -1);

	if (g_constants.InputMipLevel)
	{
		/*
		 * Apply weighted distribution:
		 * 0.5 + 0.125 + 0.125 + 0.125 + 0.125 = 1
		 * a, b, d, e * 0.125
		 * b, c, e, f * 0.125
		 * d, e, g, h * 0.125
		 * e, f, h, i * 0.125
		 * j, k, l, m * 0.5
		 * This shows 5 square areas that are being sampled. But some of them overlap,
		 * so to have an energy preserving downsample we need to make some adjustments.
		 * The weights are the distributed, so that the sum of j, k, l, m (e.g.)
		 * contribute 0.5 to the final color output. The code below is written to
		 * effectively yield this sum: 0.125 * 5 + 0.03125 * 4 + 0.0625 * 4 = 1
		 */
		return e * 0.125f + (a + c + g + i) * 0.03125f + (b + d + f + h) * 0.0625f + (j + k + l + m) * 0.125f;
	}
	
	/*
	 * We need to apply Karis average to each block of 4 samples to prevent fireflies
	 * (very bright subpixels, leads to pulsating artifacts)
	 */
	const float3 v0 = 0.125f / 4, v1 = 0.5f / 4;
	float3 groups[] =
	{
		(a + b + d + e) * v0,
		(b + c + e + f) * v0,
		(d + e + g + h) * v0,
		(e + f + h + i) * v0,
		(j + k + l + m) * v1
	};
	groups[0] *= KarisAverage(groups[0]);
	groups[1] *= KarisAverage(groups[1]);
	groups[2] *= KarisAverage(groups[2]);
	groups[3] *= KarisAverage(groups[3]);
	groups[4] *= KarisAverage(groups[4]);
	return max(groups[0] + groups[1] + groups[2] + groups[3] + groups[4], 1e-4f);
}

float3 Upsample()
{
	/*
	 * Take 9 samples around current texel 'e':
	 * a - b - c
	 * d - e - f
	 * g - h - i
	 */
	const float3
		a = Sample(-1, 1), b = Sample(0, 1), c = Sample(1, 1),
		d = Sample(-1, 0), e = Sample(0, 0), f = Sample(1, 0),
		g = Sample(-1, -1), h = Sample(0, -1), i = Sample(1, -1);

	/*
	 * Apply weighted distribution, by using a 3x3 tent filter:
	 *  1   | 1 2 1 |
	 * -- * | 2 4 2 |
	 * 16   | 1 2 1 |
	 */
	return (e * 4 + (b + d + f + h) * 2 + a + c + g + i) / 16;
}

[RootSignature(
	"StaticSampler(s0, filter=FILTER_MIN_MAG_MIP_LINEAR, addressU=TEXTURE_ADDRESS_CLAMP, addressV=TEXTURE_ADDRESS_CLAMP),"
	"RootConstants(num32BitConstants=2, b0),"
	"DescriptorTable(SRV(t0)),"
	"DescriptorTable(UAV(u0))"
)]
[numthreads(16, 16, 1)]
void main(uint2 pixelPosition : SV_DispatchThreadID)
{
	uint2 pixelDimensions;
	g_output.GetDimensions(pixelDimensions.x, pixelDimensions.y);
	if (any(pixelPosition >= pixelDimensions))
	{
		return;
	}

	g_UV = Math::CalculateUV(pixelPosition, pixelDimensions);

	float3 ret;
	if (g_constants.UpsamplingFilterRadius > 0)
	{
		g_size = g_constants.UpsamplingFilterRadius;
		ret = Upsample();
	}
	else
	{
		g_size = 1.0f / pixelDimensions;
		ret = Downsample();
	}
	g_output[pixelPosition] = ret;
}
