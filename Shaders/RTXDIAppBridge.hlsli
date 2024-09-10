#pragma once

#define RTXDI_PRESAMPLING_GROUP_SIZE 256
#define RTXDI_SCREEN_SPACE_GROUP_SIZE 8

#define RTXDI_REGIR_MODE RTXDI_REGIR_ONION

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
	bool IsReGIRCellVisualizationEnabled;
	uint _;
	struct
	{
		RTXDI_RISBufferSegmentParameters LocalLightRISBufferSegment, EnvironmentLightRISBufferSegment;
		RTXDI_LightBufferParameters LightBuffer;
		RTXDI_RuntimeParameters Runtime;
		ReGIR_Parameters ReGIR;
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

Texture2D g_localLightPDF : register(t6);
Texture2D<float> g_previousLinearDepth : register(t7);
Texture2D<float> g_linearDepth : register(t8);
Texture2D<float3> g_motionVectors : register(t9);
Texture2D<float4> g_previousBaseColorMetalness : register(t10);
Texture2D<float4> g_baseColorMetalness : register(t11);
Texture2D<float4> g_previousNormals : register(t12);
Texture2D<float4> g_normals : register(t13);
Texture2D<float> g_previousRoughness : register(t14);
Texture2D<float> g_roughness : register(t15);

RWStructuredBuffer<uint2> g_RIS : register(u0);
RWStructuredBuffer<uint4> g_RISLightInfo : register(u1);
RWStructuredBuffer<RTXDI_PackedDIReservoir> g_DIReservoir : register(u2);

RWTexture2D<float3> g_color : register(u3);
RWTexture2D<float4> g_noisyDiffuse : register(u4);
RWTexture2D<float4> g_noisySpecular : register(u5);

#define RTXDI_RIS_BUFFER g_RIS
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
		"DescriptorTable(SRV(t15))," \
		"UAV(u0)," \
		"UAV(u1)," \
		"UAV(u2)," \
		"DescriptorTable(UAV(u3))," \
		"DescriptorTable(UAV(u4))," \
		"DescriptorTable(UAV(u5))" \
	)]

#include "RaytracingHelpers.hlsli"

int2 RAB_ClampSamplePositionIntoView(int2 pixelPosition, bool previousFrame)
{
	// Reflect the position across the screen edges.
	// Compared to simple clamping, this prevents the spread of colorful blobs from screen edges.
	const int2 pixelDimensions = (int2)g_graphicsSettings.RenderSize;
	if (pixelPosition.x < 0)
	{
		pixelPosition.x = -pixelPosition.x;
	}
	if (pixelPosition.y < 0)
	{
		pixelPosition.y = -pixelPosition.y;
	}
	if (pixelPosition.x >= pixelDimensions.x)
	{
		pixelPosition.x = 2 * pixelDimensions.x - pixelPosition.x - 1;
	}
	if (pixelPosition.y >= pixelDimensions.y)
	{
		pixelPosition.y = 2 * pixelDimensions.y - pixelPosition.y - 1;
	}
	return pixelPosition;
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

RAB_LightInfo RAB_LoadCompactLightInfo(uint linearIndex)
{
	RAB_LightInfo lightInfo;
	lightInfo.Load(g_RISLightInfo[linearIndex * 2], g_RISLightInfo[linearIndex * 2 + 1]);
	return lightInfo;
}

bool RAB_StoreCompactLightInfo(uint linearIndex, RAB_LightInfo lightInfo)
{
	return lightInfo.Store(g_RISLightInfo[linearIndex * 2], g_RISLightInfo[linearIndex * 2 + 1]);
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
	float3 Position, Normal, GeometricNormal, ViewDirection;
	float LinearDepth;
	BRDFSample BRDFSample;
	float DiffuseProbability;

	bool Sample(inout RAB_RandomSamplerState rng, out float3 L)
	{
		const float3x3 basis = Geometry::GetBasis(Normal);
		const float2 randomValue = float2(RAB_GetNextRandom(rng), RAB_GetNextRandom(rng));
		if (RAB_GetNextRandom(rng) < DiffuseProbability)
		{
			L = Geometry::RotateVectorInverse(basis, ImportanceSampling::Cosine::GetRay(randomValue));
		}
		else
		{
			L = reflect(-ViewDirection, Geometry::RotateVectorInverse(basis, ImportanceSampling::VNDF::GetRay(randomValue, max(BRDFSample.Roughness, MinRoughness), Geometry::RotateVector(basis, ViewDirection))));
		}
		return dot(Normal, L) > 0;
	}

	float GetPDF(float3 L)
	{
		const float NoL = dot(Normal, L);
		if (NoL > 0)
		{
			const float
				NoV = abs(dot(Normal, ViewDirection)),
				NoH = abs(dot(Normal, normalize(ViewDirection + L))),
				diffusePDF = ImportanceSampling::Cosine::GetPDF(NoL),
				specularPDF = ImportanceSampling::VNDF::GetPDF(Geometry::RotateVector(Geometry::GetBasis(Normal), ViewDirection), NoH, max(BRDFSample.Roughness, MinRoughness));
			return lerp(specularPDF, diffusePDF, DiffuseProbability);
		}
		return 0;
	}

	bool Evaluate(float3 L, out float3 diffuse, out float3 specular, float minRoughness = MinRoughness)
	{
		if (dot(GeometricNormal, L) > 0)
		{
			const float3 H = normalize(ViewDirection + L);
			const float
				NoL = abs(dot(Normal, L)),
				NoV = abs(dot(Normal, ViewDirection)),
				VoH = abs(dot(ViewDirection, H)),
				NoH = abs(dot(Normal, H)),
				roughness = max(BRDFSample.Roughness, minRoughness);
			const float3 F = BRDF::FresnelTerm(BRDFSample.Rf0, VoH);
			diffuse = (1 - F) * BRDF::DiffuseTerm(roughness, NoL, NoV, VoH) * BRDFSample.Albedo * NoL;
			specular = F * BRDF::DistributionTerm(roughness, NoH) * BRDF::GeometryTermMod(roughness, NoL, NoV, VoH, NoH) * NoL;
			return true;
		}
		diffuse = specular = 0;
		return false;
	}

	bool Shade(float3 samplePosition, float3 radiance, out float3 diffuse, out float3 specular)
	{
		const bool ret = Evaluate(normalize(samplePosition - Position), diffuse, specular);
		if (ret)
		{
			diffuse *= radiance;
			specular *= radiance;
		}
		return ret;
	}

	bool Shade(RAB_LightSample lightSample, out float3 diffuse, out float3 specular)
	{
		if (lightSample.SolidAnglePDF <= 0)
		{
			diffuse = specular = 0;
			return false;
		}
		const bool ret = Shade(lightSample.Position, lightSample.Radiance, diffuse, specular);
		if (ret)
		{
			const float invPDF = 1 / lightSample.SolidAnglePDF;
			diffuse *= invPDF;
			specular *= invPDF;
		}
		return ret;
	}
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
		float4 normals, baseColorMetalness;
		float roughness;
		if (previousFrame)
		{
			if (isinf(surface.LinearDepth = g_previousLinearDepth[pixelPosition]))
			{
				return surface;
			}
			normals = g_previousNormals[pixelPosition];
			baseColorMetalness = g_previousBaseColorMetalness[pixelPosition];
			roughness = g_previousRoughness[pixelPosition];
		}
		else
		{
			if (isinf(surface.LinearDepth = g_linearDepth[pixelPosition]))
			{
				return surface;
			}
			normals = g_normals[pixelPosition];
			baseColorMetalness = g_baseColorMetalness[pixelPosition];
			roughness = g_roughness[pixelPosition];
		}
		surface.Position = g_camera.ReconstructWorldPosition(Math::CalculateNDC(Math::CalculateUV(pixelPosition, g_graphicsSettings.RenderSize, g_camera.Jitter)), surface.LinearDepth, previousFrame);
		surface.Normal = Packing::DecodeUnitVector(normals.xy, true);
		surface.GeometricNormal = Packing::DecodeUnitVector(normals.zw, true);
		surface.ViewDirection = normalize(g_camera.Position - surface.Position);
		surface.BRDFSample.Initialize(baseColorMetalness.rgb, baseColorMetalness.a, roughness);
		surface.DiffuseProbability = EstimateDiffuseProbability(surface.BRDFSample.Albedo, surface.BRDFSample.Rf0, max(surface.BRDFSample.Roughness, MinRoughness), abs(dot(surface.Normal, surface.ViewDirection)));
	}
	return surface;
}

bool RAB_IsSurfaceValid(RAB_Surface surface)
{
	return !isinf(surface.LinearDepth);
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
	return RTXDI_CompareRelativeDifference(a.BRDFSample.Roughness, b.BRDFSample.Roughness, 0.5f)
		&& abs(Color::Luminance(a.BRDFSample.Rf0) - Color::Luminance(b.BRDFSample.Rf0)) <= 0.25f
		&& abs(Color::Luminance(a.BRDFSample.Albedo) - Color::Luminance(b.BRDFSample.Albedo)) <= 0.25f;
}

void RAB_GetLightDirDistance(RAB_Surface surface, RAB_LightSample lightSample, out float3 o_lightDir, out float o_lightDistance)
{
	o_lightDir = lightSample.Position - surface.Position;
	o_lightDistance = length(o_lightDir);
	o_lightDir /= o_lightDistance;
}

bool RAB_GetSurfaceBrdfSample(RAB_Surface surface, inout RAB_RandomSamplerState rng, out float3 dir)
{
	return surface.Sample(rng, dir);
}

float RAB_GetSurfaceBrdfPdf(RAB_Surface surface, float3 dir)
{
	return surface.GetPDF(dir);
}

float RAB_GetLightSampleTargetPdfForSurface(RAB_LightSample lightSample, RAB_Surface surface)
{
	float3 diffuse, specular;
	surface.Shade(lightSample, diffuse, specular);
	return Color::Luminance(diffuse + specular);
}

float RAB_GetLightTargetPdfForVolume(RAB_LightInfo light, float3 volumeCenter, float volumeRadius)
{
	TriangleLight triangleLight;
	triangleLight.Load(light);
	return triangleLight.CalculateWeightForVolume(volumeCenter, volumeRadius);
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
		o_lightIndex = g_lightIndices[q.CommittedInstanceID() + q.CommittedGeometryIndex()];
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
	uint mipLevels;
	uint2 textureSize;
	g_localLightPDF.GetDimensions(0, textureSize.x, textureSize.y, mipLevels);

	// The single texel in the last mip level is effectively the average of all texels in mip 0,
	// padded to a square shape with zeros. So, in case the PDF texture has a 2:1 aspect ratio,
	// that texel's value is only half of the true average of the rectangular input texture.
	// Compensate for that by assuming that the input texture is square.
	const uint value = 1u << (mipLevels - 1);
	const float sum = g_localLightPDF.mips[mipLevels - 1][(uint2)0].x * value * value;
	return g_localLightPDF[RTXDI_LinearIndexToZCurve(lightIndex)].x / sum;
}

float2 RAB_GetEnvironmentMapRandXYFromDir(float3 worldDir)
{
	return 0;
}

float RAB_EvaluateEnvironmentMapSamplingPdf(float3 L)
{
	return 0;
}
