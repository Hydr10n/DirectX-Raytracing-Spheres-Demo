#pragma once

#include "Math.hlsli"

struct LightInfo
{
	float3 Base, Edges[2], Radiance;
};

struct LightSample
{
	float3 Position, Normal, Radiance;
	float SolidAnglePDF;
};

float CalculateAverageDistanceToVolume(float distanceToCenter, float volumeRadius)
{
	// The expression and factor are fitted to a Monte Carlo estimated curve.
	// At distanceToCenter == 0, this function returns (0.75 * volumeRadius) which is analytically accurate.
	// At infinity, the result asymptotically approaches distanceToCenter.
	const float nonlinearFactor = 1.1547f, value = distanceToCenter + volumeRadius * nonlinearFactor;
	return distanceToCenter + volumeRadius * volumeRadius * volumeRadius / (value * value);
}

struct TriangleLight
{
	float3 Base, Edges[2], Normal, Radiance;
	float Area;

	void Initialize(float3 base, float3 edge0, float3 edge1, float3 radiance)
	{
		Base = base;
		Edges[0] = edge0;
		Edges[1] = edge1;
		const float3 normal = cross(edge0, edge1);
		const float normalLength = length(normal);
		if (normalLength > 0)
		{
			Normal = normal / normalLength;
			Area = normalLength / 2;
		}
		else
		{
			Normal = 0;
			Area = 0;
		}
		Radiance = radiance;
	}

	void Load(LightInfo lightInfo)
	{
		Initialize(lightInfo.Base, lightInfo.Edges[0], lightInfo.Edges[1], lightInfo.Radiance);
	}

	void Store(out LightInfo lightInfo)
	{
		lightInfo.Base = Base;
		lightInfo.Edges = Edges;
		lightInfo.Radiance = Radiance;
	}

	float CalculateSolidAnglePDF(float3 viewPosition, float3 lightSamplePosition, float3 lightSampleNormal)
	{
		const float3 L = lightSamplePosition - viewPosition;
		const float Llength = length(L);
		return Math::ToSolidAnglePDF(1 / Area, Llength, abs(dot(L / Llength, -lightSampleNormal)));
	}

	LightSample CalculateSample(float3 viewPosition, float2 random)
	{
		LightSample lightSample;
		const float3 barycentrics = Math::SampleTriangle(random);
		lightSample.Position = Base + Edges[0] * barycentrics.y + Edges[1] * barycentrics.z;
		lightSample.Normal = Normal;
		lightSample.Radiance = Radiance;
		lightSample.SolidAnglePDF = CalculateSolidAnglePDF(viewPosition, lightSample.Position, lightSample.Normal);
		return lightSample;
	}

	float CalculatePower()
	{
		return Area * Math::Pi(1) * Color::Luminance(Radiance);
	}

	float CalculateWeightForVolume(float3 volumeCenter, float volumeRadius)
	{
		if (dot(volumeCenter - Base, Normal) < -volumeRadius)
		{
			return 0;
		}
		const float
			distance = CalculateAverageDistanceToVolume(length(Base + (Edges[0] + Edges[1]) / 3 - volumeCenter), volumeRadius),
			approximateSolidAngle = min(Area / (distance * distance), Math::Pi(2));
		return approximateSolidAngle * Color::Luminance(Radiance);
	}
};
