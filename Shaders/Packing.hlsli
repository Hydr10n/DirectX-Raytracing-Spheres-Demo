#pragma once

int16_t Pack_R16_SNORM(float value)
{
	return int16_t(clamp(value, -1, 1) * 0x7fff);
}

float Unpack_R16_SNORM(int16_t value)
{
	return max(float(value) / 0x7fff, -1);
}

int16_t2 Pack_R16G16_SNORM(float2 value)
{
	return int16_t2(Pack_R16_SNORM(value.r), Pack_R16_SNORM(value.g));
}

float2 Unpack_R16G16_SNORM(int16_t2 value)
{
	return float2(Unpack_R16_SNORM(value.r), Unpack_R16_SNORM(value.g));
}

int16_t3 Pack_R16G16B16_SNORM(float3 value)
{
	return int16_t3(Pack_R16G16_SNORM(value.rg), Pack_R16_SNORM(value.b));
}

float3 Unpack_R16G16B16_SNORM(int16_t3 value)
{
	return float3(Unpack_R16G16_SNORM(value.rg), Unpack_R16_SNORM(value.b));
}

int16_t4 Pack_R16G16B16A16_SNORM(float4 value)
{
	return int16_t4(Pack_R16G16B16_SNORM(value.rgb), Pack_R16_SNORM(value.a));
}

float4 Unpack_R16G16B16A16_SNORM(int16_t4 value)
{
	return float4(Unpack_R16G16B16_SNORM(value.rgb), Unpack_R16_SNORM(value.a));
}
