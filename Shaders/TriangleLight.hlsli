#pragma once

#include "Math.hlsli"

struct RAB_LightInfo {
	float3 Center;
	uint Scalars, Directions[2];
	uint2 Radiance;
};

RAB_LightInfo RAB_EmptyLightInfo() { return (RAB_LightInfo)0; }

struct RAB_LightSample {
	float3 Position, Normal, Radiance;
	float SolidAnglePDF;
};

RAB_LightSample RAB_EmptyLightSample() { return (RAB_LightSample)0; }
bool RAB_IsAnalyticLightSample(RAB_LightSample lightSample) { return false; }
float RAB_LightSampleSolidAnglePdf(RAB_LightSample lightSample) { return lightSample.SolidAnglePDF; }

struct TriangleLight {
	float3 Base, Edges[2], Normal, Radiance;
	float Area;

	void Load(RAB_LightInfo lightInfo) {
		const float2 scalars = STL::Packing::UintToRg16f(lightInfo.Scalars);
		Edges[0] = STL::Packing::DecodeUnitVector(STL::Packing::UintToRg16f(lightInfo.Directions[0]), true) * scalars[0];
		Edges[1] = STL::Packing::DecodeUnitVector(STL::Packing::UintToRg16f(lightInfo.Directions[1]), true) * scalars[1];
		Base = lightInfo.Center - (Edges[0] + Edges[1]) / 3;
		Radiance = float3(STL::Packing::UintToRg16f(lightInfo.Radiance.x), STL::Packing::UintToRg16f(lightInfo.Radiance.y).x);
		const float3 normal = cross(Edges[0], Edges[1]);
		const float normalLength = length(normal);
		if (normalLength > 0) {
			Normal = normal / normalLength;
			Area = normalLength / 2;
		}
		else {
			Normal = 0;
			Area = 0;
		}
	}

	RAB_LightInfo Store() {
		RAB_LightInfo lightInfo;
		lightInfo.Center = Base + (Edges[0] + Edges[1]) / 3;
		const float2 scalars = float2(length(Edges[0]), length(Edges[1]));
		lightInfo.Scalars = STL::Packing::Rg16fToUint(scalars);
		lightInfo.Directions[0] = STL::Packing::Rg16fToUint(STL::Packing::EncodeUnitVector(Edges[0] / scalars[0], true));
		lightInfo.Directions[1] = STL::Packing::Rg16fToUint(STL::Packing::EncodeUnitVector(Edges[1] / scalars[1], true));
		lightInfo.Radiance = uint2(STL::Packing::Rg16fToUint(Radiance.rg), STL::Packing::Rg16fToUint(float2(Radiance.b, 0)));
		return lightInfo;
	}

	float CalculateSolidAnglePDF(float3 viewPosition, float3 lightSamplePosition, float3 lightSampleNormal) {
		const float3 L = lightSamplePosition - viewPosition;
		const float Llength = length(L);
		return Math::ToSolidAnglePDF(1 / Area, Llength, abs(dot(L / Llength, -lightSampleNormal)));
	}

	RAB_LightSample CalculateSample(float2 randomValue, float3 viewPosition) {
		RAB_LightSample lightSample;
		const float3 barycentrics = Math::SampleTriangle(randomValue);
		lightSample.Position = Base + Edges[0] * barycentrics.y + Edges[1] * barycentrics.z;
		lightSample.Normal = Normal;
		lightSample.Radiance = Radiance;
		lightSample.SolidAnglePDF = CalculateSolidAnglePDF(viewPosition, lightSample.Position, lightSample.Normal);
		return lightSample;
	}
};
