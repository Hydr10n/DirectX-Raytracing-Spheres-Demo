#pragma once

#include "ml.hlsli"

namespace Math
{
	float2 CalculateUV(float2 pixelPosition, float2 pixelDimensions, float2 cameraJitter = 0)
	{
		return (pixelPosition + 0.5f + cameraJitter) / pixelDimensions;
	}

	float2 CalculateNDC(float2 UV)
	{
		return UV * float2(2, -2) + float2(-1, 1);
	}

	float3x3 CalculateTBN(float3 N, float3 T)
	{
		T = normalize(T - N * dot(N, T));
		return float3x3(T, cross(N, T), N);
	}

	float3x3 InverseTranspose(float3x3 m)
	{
		const float3 v = cross(m[0], m[1]);
		return float3x3(cross(m[1], m[2]), cross(m[2], m[0]), v) / dot(v, m[2]);
	}

	float2 ToLatLongCoordinate(float3 direction)
	{
		const float Pi = Math::Pi(1);
		return float2((1 + atan2(direction.x, direction.z) / Pi) / 2, acos(direction.y) / Pi);
	}

	float2 RandomFromBarycentrics(float2 barycentrics)
	{
		const float value = barycentrics.x + barycentrics.y;
		return float2(value * value, barycentrics.y / value);
	}

	float2 SampleTriangle(float2 random)
	{
		const float value = sqrt(random.x);
		return float2(value * (1 - random.y), value * random.y);
	}

	float ToSolidAnglePDF(float areaPDF, float length, float cosTheta)
	{
		return areaPDF * length * length / cosTheta;
	}
}
