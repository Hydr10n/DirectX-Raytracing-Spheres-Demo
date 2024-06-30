#pragma once

#include "rtxdi/RtxdiMath.hlsli"

#include "Math.hlsli"

struct LightInfo
{
	float3 Center;
	uint Scalars;
	uint2 Directions, Radiance;
	
	void Load(uint4 data0, uint4 data1)
	{
		Center = asfloat(data0.xyz);
		Scalars = data0.w;
		Directions = data1.xy;
		Radiance = data1.zw;
	}
	
	bool Store(out uint4 data0, out uint4 data1)
	{
		data0.xyz = asuint(Center);
		data0.w = Scalars;
		data1.xy = Directions;
		data1.zw = Radiance;
		return true;
	}
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
		const float2 scalars = Packing::UintToRg16f(lightInfo.Scalars);
		const float3
			edge0 = RTXDI_DecodeNormalizedVectorFromSnorm2x16(lightInfo.Directions[0]) * scalars[0],
			edge1 = RTXDI_DecodeNormalizedVectorFromSnorm2x16(lightInfo.Directions[1]) * scalars[1],
			base = lightInfo.Center - (edge0 + edge1) / 3,
			radiance = float3(Packing::UintToRg16f(lightInfo.Radiance.x), Packing::UintToRg16f(lightInfo.Radiance.y).x);
		Initialize(base, edge0, edge1, radiance);
	}

	void Store(out LightInfo lightInfo)
	{
		lightInfo.Center = Base + (Edges[0] + Edges[1]) / 3;
		const float2 scalars = float2(length(Edges[0]), length(Edges[1]));
		lightInfo.Scalars = Packing::Rg16fToUint(scalars);
		lightInfo.Directions[0] = RTXDI_EncodeNormalizedVectorToSnorm2x16(Edges[0] / scalars[0]);
		lightInfo.Directions[1] = RTXDI_EncodeNormalizedVectorToSnorm2x16(Edges[1] / scalars[1]);
		lightInfo.Radiance = uint2(Packing::Rg16fToUint(Radiance.rg), Packing::Rg16fToUint(float2(Radiance.b, 0)));
	}

	float CalculateSolidAnglePDF(float3 viewPosition, float3 lightSamplePosition, float3 lightSampleNormal)
	{
		const float3 L = lightSamplePosition - viewPosition;
		const float Llength = length(L);
		return Math::ToSolidAnglePDF(1 / Area, Llength, abs(dot(L / Llength, -lightSampleNormal)));
	}

	LightSample CalculateSample(float3 viewPosition, float2 randomValue)
	{
		LightSample lightSample;
		const float3 barycentrics = Math::SampleTriangle(randomValue);
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
