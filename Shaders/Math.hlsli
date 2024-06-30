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

	float3 CalculateTangent(float3 positions[3], float2 textureCoordinates[3])
	{
		const float2 d1 = textureCoordinates[1] - textureCoordinates[0], d2 = textureCoordinates[2] - textureCoordinates[0];
		return ((positions[1] - positions[0]) * d2.y - (positions[2] - positions[0]) * d1.y) / (d1.x * d2.y - d1.y * d2.x);
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

	float3 SampleTriangle(float2 randomValue)
	{
		const float value = sqrt(randomValue.x);
		return float3(1 - value, value * (1 - randomValue.y), value * randomValue.y);
	}

	float ToSolidAnglePDF(float AreaPDF, float _length, float cosTheta)
	{
		return AreaPDF * _length * _length / cosTheta;
	}
}
