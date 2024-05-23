#pragma once

#define RTXDI_ENABLE_PRESAMPLING 0

#define RTXDI_SCREEN_SPACE_GROUP_SIZE 8

#define RTXDI_ENABLE_BOILING_FILTER
#define RTXDI_BOILING_FILTER_GROUP_SIZE RTXDI_SCREEN_SPACE_GROUP_SIZE

#include "rtxdi/ReSTIRDIParameters.h"

#include "Common.hlsli"

#include "Camera.hlsli"

#include "Light.hlsli"

#include "Denoiser.hlsli"

SamplerState g_anisotropicSampler : register(s0);

RaytracingAccelerationStructure g_scene : register(t0);

struct GraphicsSettings
{
	uint2 RenderSize;
	uint FrameIndex, _;
	struct
	{
		RTXDI_LightBufferParameters LightBuffer;
		RTXDI_RuntimeParameters Runtime;
		ReSTIRDI_Parameters ReSTIRDI;
	} RTXDI;
	NRDSettings NRD;
};
ConstantBuffer<GraphicsSettings> g_graphicsSettings : register(b0);

ConstantBuffer<Camera> g_camera : register(b1);

StructuredBuffer<InstanceData> g_instanceData : register(t1);
StructuredBuffer<ObjectData> g_objectData : register(t2);
StructuredBuffer<LightInfo> g_lightInfo : register(t3);
StructuredBuffer<uint> g_lightIndices : register(t4);
Buffer<float2> g_neighborOffsets : register(t5);

Texture2D<float> g_previousLinearDepth : register(t6);
Texture2D<float> g_linearDepth : register(t7);
Texture2D<float3> g_motionVectors : register(t8);
Texture2D<float4> g_previousBaseColorMetalness : register(t9);
Texture2D<float4> g_baseColorMetalness : register(t10);
Texture2D<float4> g_previousNormals : register(t11);
Texture2D<float4> g_normals : register(t12);
Texture2D<float> g_previousRoughness : register(t13);
Texture2D<float> g_roughness : register(t14);

RWStructuredBuffer<RTXDI_PackedDIReservoir> g_DIReservoir : register(u0);

RWTexture2D<float3> g_color : register(u1);
RWTexture2D<float4> g_noisyDiffuse : register(u2);
RWTexture2D<float4> g_noisySpecular : register(u3);

#define RTXDI_NEIGHBOR_OFFSETS_BUFFER g_neighborOffsets
#define RTXDI_LIGHT_RESERVOIR_BUFFER g_DIReservoir

#define ROOT_SIGNATURE \
	[RootSignature( \
		"RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
		"StaticSampler(s0)," \
		"SRV(t0)," \
		"CBV(b0)," \
		"CBV(b1)," \
		"SRV(t1)," \
		"SRV(t2)," \
		"SRV(t3)," \
		"SRV(t4)," \
		"DescriptorTable(SRV(t5))," \
		"DescriptorTable(SRV(t6))," \
		"DescriptorTable(SRV(t7))," \
		"DescriptorTable(SRV(t8))," \
		"DescriptorTable(SRV(t9))," \
		"DescriptorTable(SRV(t10))," \
		"DescriptorTable(SRV(t11))," \
		"DescriptorTable(SRV(t12))," \
		"DescriptorTable(SRV(t13))," \
		"DescriptorTable(SRV(t14))," \
		"UAV(u0)," \
		"DescriptorTable(UAV(u1))," \
		"DescriptorTable(UAV(u2))," \
		"DescriptorTable(UAV(u3))" \
	)]

#include "RaytracingHelpers.hlsli"

int2 RAB_ClampSamplePositionIntoView(int2 pixelPosition, bool previousFrame)
{
	return clamp(pixelPosition, 0, int2(g_graphicsSettings.RenderSize) - 1);
}

struct RAB_RandomSamplerState
{
	uint Seed, Index;
};

RAB_RandomSamplerState RAB_InitRandomSampler(uint2 pixelPos, uint frameIndex)
{
	RAB_RandomSamplerState state;
	state.Seed = RTXDI_JenkinsHash(RTXDI_ZCurveToLinearIndex(pixelPos)) + frameIndex;
	state.Index = 1;
	return state;
}

float RAB_GetNextRandom(inout RAB_RandomSamplerState rng)
{
#define ROT32(x, y) ((x << y) | (x >> (32 - y)))
	// https://en.wikipedia.org/wiki/MurmurHash
	const uint c1 = 0xcc9e2d51, c2 = 0x1b873593, r1 = 15, r2 = 13, m = 5, n = 0xe6546b64;
	uint hash = rng.Seed, k = rng.Index++;
	k *= c1;
	k = ROT32(k, r1);
	k *= c2;
	hash ^= k;
	hash = ROT32(hash, r2) * m + n;
	hash ^= 4;
	hash ^= hash >> 16;
	hash *= 0x85ebca6b;
	hash ^= hash >> 13;
	hash *= 0xc2b2ae35;
	hash ^= hash >> 16;
#undef ROT32
	const uint one = asuint(1.0f), mask = (1 << 23) - 1;
	return asfloat((mask & hash) | one) - 1;
}

int RAB_TranslateLightIndex(uint lightIndex, bool currentToPrevious)
{
	return int(lightIndex);
}

using RAB_LightInfo = LightInfo;

RAB_LightInfo RAB_EmptyLightInfo()
{
	return (RAB_LightInfo)0;
}

RAB_LightInfo RAB_LoadLightInfo(uint index, bool previousFrame)
{
	return g_lightInfo[index];
}

using RAB_LightSample = LightSample;

RAB_LightSample RAB_EmptyLightSample()
{
	return (RAB_LightSample)0;
}

bool RAB_IsAnalyticLightSample(RAB_LightSample lightSample)
{
	return false;
}

float RAB_LightSampleSolidAnglePdf(RAB_LightSample lightSample)
{
	return lightSample.SolidAnglePDF;
}

struct RAB_Surface
{
	float3 Position, Normal, GeometricNormal;
	float3 Albedo, Rf0;
	float Roughness, DiffuseProbability;
	float3 ViewDirection;
	float LinearDepth;
};

RAB_Surface RAB_EmptySurface()
{
	RAB_Surface surface = (RAB_Surface)0;
	surface.LinearDepth = 1.#INF;
	return surface;
}

RAB_Surface RAB_GetGBufferSurface(int2 pixelPosition, bool previousFrame)
{
	RAB_Surface surface = RAB_EmptySurface();
	if (all(pixelPosition < g_graphicsSettings.RenderSize))
	{
		float4 baseColorMetalness, normal;
		float roughness;
		if (previousFrame)
		{
			if ((surface.LinearDepth = g_previousLinearDepth[pixelPosition]) == 1.#INF)
			{
				return surface;
			}
			baseColorMetalness = g_previousBaseColorMetalness[pixelPosition];
			normal = g_previousNormals[pixelPosition];
			roughness = g_previousRoughness[pixelPosition];
		}
		else
		{
			if ((surface.LinearDepth = g_linearDepth[pixelPosition]) == 1.#INF)
			{
				return surface;
			}
			baseColorMetalness = g_baseColorMetalness[pixelPosition];
			normal = g_normals[pixelPosition];
			roughness = g_roughness[pixelPosition];
		}
		surface.Position = g_camera.ReconstructWorldPosition(Math::CalculateNDC(Math::CalculateUV(pixelPosition, g_graphicsSettings.RenderSize, g_camera.Jitter)), surface.LinearDepth, previousFrame);
		surface.Normal = STL::Packing::DecodeUnitVector(normal.xy, true);
		surface.GeometricNormal = STL::Packing::DecodeUnitVector(normal.zw, true);
		surface.Roughness = max(roughness, MinRoughness);
		STL::BRDF::ConvertBaseColorMetalnessToAlbedoRf0(baseColorMetalness.rgb, baseColorMetalness.a, surface.Albedo, surface.Rf0);
		surface.ViewDirection = normalize(g_camera.Position - surface.Position);
		surface.DiffuseProbability = Material::EstimateDiffuseProbability(surface.Albedo, surface.Rf0, surface.Roughness, abs(dot(surface.Normal, surface.ViewDirection)));
	}
	return surface;
}

bool RAB_IsSurfaceValid(RAB_Surface surface)
{
	return surface.LinearDepth != 1.#INF;
}

float3 RAB_GetSurfaceWorldPos(RAB_Surface surface)
{
	return surface.Position;
}

float3 RAB_GetSurfaceNormal(RAB_Surface surface)
{
	return surface.Normal;
}

float3 RAB_GetSurfaceViewDir(RAB_Surface surface)
{
	return surface.ViewDirection;
}

float RAB_GetSurfaceLinearDepth(RAB_Surface surface)
{
	return surface.LinearDepth;
}

bool RAB_AreMaterialsSimilar(RAB_Surface a, RAB_Surface b)
{
	return RTXDI_CompareRelativeDifference(a.Roughness, b.Roughness, 0.5f)
		&& abs(STL::Color::Luminance(a.Rf0) - STL::Color::Luminance(b.Rf0)) <= 0.25f
		&& abs(STL::Color::Luminance(a.Albedo) - STL::Color::Luminance(b.Albedo)) <= 0.25f;
}

void RAB_GetLightDirDistance(RAB_Surface surface, RAB_LightSample lightSample, out float3 o_lightDir, out float o_lightDistance)
{
	o_lightDir = lightSample.Position - surface.Position;
	o_lightDistance = length(o_lightDir);
	o_lightDir /= o_lightDistance;
}

bool RAB_GetSurfaceBrdfSample(RAB_Surface surface, inout RAB_RandomSamplerState rng, out float3 dir)
{
	const float3x3 basis = STL::Geometry::GetBasis(surface.Normal);
	const float2 randomValue = float2(RAB_GetNextRandom(rng), RAB_GetNextRandom(rng));
	if (RAB_GetNextRandom(rng) < surface.DiffuseProbability)
	{
		dir = STL::Geometry::RotateVectorInverse(basis, STL::ImportanceSampling::Cosine::GetRay(randomValue));
	}
	else
	{
		dir = reflect(-surface.ViewDirection, STL::Geometry::RotateVectorInverse(basis, STL::ImportanceSampling::VNDF::GetRay(randomValue, surface.Roughness, STL::Geometry::RotateVector(basis, surface.ViewDirection))));
	}
	return dot(surface.Normal, dir) > 0;
}

bool EvaluateBRDF(float3 L, RAB_Surface surface, out float3 diffuse, out float3 specular)
{
	if (dot(surface.GeometricNormal, L) > 0)
	{
		const float3 H = normalize(surface.ViewDirection + L);
		const float NoL = abs(dot(surface.Normal, L)), NoV = abs(dot(surface.Normal, surface.ViewDirection)), VoH = abs(dot(surface.ViewDirection, H)), NoH = abs(dot(surface.Normal, H));
		const float3 F = STL::BRDF::FresnelTerm(surface.Rf0, VoH);
		diffuse = (1 - F) * STL::BRDF::DiffuseTerm(surface.Roughness, NoL, NoV, VoH) * NoL;
		specular = F * STL::BRDF::DistributionTerm(surface.Roughness, NoH) * STL::BRDF::GeometryTermMod(surface.Roughness, NoL, NoV, VoH, NoH) * NoL;
		return true;
	}
	diffuse = specular = 0;
	return false;
}

bool ShadeSurface(float3 samplePosition, float3 radiance, RAB_Surface surface, out float3 diffuse, out float3 specular)
{
	const bool ret = EvaluateBRDF(normalize(samplePosition - surface.Position), surface, diffuse, specular);
	if (ret)
	{
		diffuse *= surface.Albedo * radiance;
		specular *= radiance;
	}
	return ret;
}

void ShadeSurface(RAB_LightSample lightSample, RAB_Surface surface, out float3 diffuse, out float3 specular)
{
	if (lightSample.SolidAnglePDF <= 0)
	{
		diffuse = specular = 0;
		return;
	}
	if (ShadeSurface(lightSample.Position, lightSample.Radiance, surface, diffuse, specular))
	{
		const float invPDF = 1 / lightSample.SolidAnglePDF;
		diffuse *= invPDF;
		specular *= invPDF;
	}
}

float RAB_GetSurfaceBrdfPdf(RAB_Surface surface, float3 dir)
{
	const float NoL = dot(surface.Normal, dir);
	if (NoL > 0)
	{
		const float
		NoV = abs(dot(surface.Normal, surface.ViewDirection)), NoH = abs(dot(surface.Normal, normalize(surface.ViewDirection + dir))),
		diffusePDF = STL::ImportanceSampling::Cosine::GetPDF(NoL),
		specularPDF = STL::ImportanceSampling::VNDF::GetPDF(STL::Geometry::RotateVector(STL::Geometry::GetBasis(surface.Normal), surface.ViewDirection), NoH, surface.Roughness);
		return lerp(specularPDF, diffusePDF, surface.DiffuseProbability);
	}
	return 0;
}

float RAB_GetLightSampleTargetPdfForSurface(RAB_LightSample lightSample, RAB_Surface surface)
{
	float3 diffuse, specular;
	ShadeSurface(lightSample, surface, diffuse, specular);
	return STL::Color::Luminance(diffuse + specular);
}

bool GetConservativeVisibility(RAB_Surface surface, float3 samplePosition)
{
	const float3 L = samplePosition - surface.Position;
	const float Llength = length(L);
	const RayDesc rayDesc = { surface.Position, 1e-3f, L / Llength, max(1e-3f, Llength - 1e-3f) };
	RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
	TraceRay(q, rayDesc, RAY_FLAG_NONE, ~0u);
	return q.CommittedStatus() == COMMITTED_NOTHING;
}

bool GetFinalVisibility(RAB_Surface surface, float3 samplePosition)
{
	return GetConservativeVisibility(surface, samplePosition);
}

bool RAB_GetConservativeVisibility(RAB_Surface surface, float3 samplePosition)
{
	return GetConservativeVisibility(surface, samplePosition);
}

bool RAB_GetConservativeVisibility(RAB_Surface surface, RAB_LightSample lightSample)
{
	return RAB_GetConservativeVisibility(surface, lightSample.Position);
}

bool RAB_GetTemporalConservativeVisibility(RAB_Surface currentSurface, RAB_Surface previousSurface, float3 samplePosition)
{
	return RAB_GetConservativeVisibility(currentSurface, samplePosition);
}

bool RAB_GetTemporalConservativeVisibility(RAB_Surface currentSurface, RAB_Surface previousSurface, RAB_LightSample lightSample)
{
	return RAB_GetTemporalConservativeVisibility(currentSurface, previousSurface, lightSample.Position);
}

RAB_LightSample RAB_SamplePolymorphicLight(RAB_LightInfo lightInfo, RAB_Surface surface, float2 uv)
{
	TriangleLight triangleLight;
	triangleLight.Load(lightInfo);
	return triangleLight.CalculateSample(surface.Position, uv);
}

bool RAB_TraceRayForLocalLight(float3 origin, float3 direction, float tMin, float tMax, out uint o_lightIndex, out float2 o_randXY)
{
	const RayDesc rayDesc = { origin, tMin, direction, tMax };
	RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
	TraceRay(q, rayDesc, RAY_FLAG_NONE, ~0u);
	const bool hit = q.CommittedStatus() != COMMITTED_NOTHING;
	if (hit)
	{
		o_lightIndex = g_lightIndices[g_instanceData[q.CommittedInstanceIndex()].FirstGeometryIndex + q.CommittedGeometryIndex()];
		if (o_lightIndex != RTXDI_InvalidLightIndex)
		{
			o_lightIndex += q.CommittedPrimitiveIndex();
			o_randXY = Math::RandomFromBarycentrics(q.CommittedTriangleBarycentrics());
		}
	}
	else
	{
		o_lightIndex = RTXDI_InvalidLightIndex;
		o_randXY = 0;
	}
	return hit;
}

float RAB_EvaluateLocalLightSourcePdf(uint lightIndex)
{
	return 1.0f / g_graphicsSettings.RTXDI.LightBuffer.localLightBufferRegion.numLights;
}

float2 RAB_GetEnvironmentMapRandXYFromDir(float3 worldDir)
{
	return 0;
}

float RAB_EvaluateEnvironmentMapSamplingPdf(float3 L)
{
	return 0;
}
