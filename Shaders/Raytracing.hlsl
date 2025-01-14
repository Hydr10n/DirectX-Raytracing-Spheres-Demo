#include "Common.hlsli"

#include "Camera.hlsli"

#include "Denoiser.hlsli"

#if defined(SHARC_UPDATE) || defined(SHARC_QUERY)
#define ENABLE_SHARC 1
#include "SharcCommon.h"
#else
#define ENABLE_SHARC 0
#endif

#define NV_SHADER_EXTN_SLOT u1024
#include "nvHLSLExtns.h"

SamplerState g_anisotropicSampler : register(s0);

RaytracingAccelerationStructure g_scene : register(t0);

struct GraphicsSettings
{
	uint FrameIndex, Bounces, SamplesPerPixel;
	float ThroughputThreshold;
	bool IsRussianRouletteEnabled, IsShaderExecutionReorderingEnabled, IsSecondarySurfaceEmissionIncluded;
	uint _;
	struct
	{
		struct
		{
			uint Capacity;
			float SceneScale, RoughnessThreshold;
			bool IsAntiFileflyEnabled, IsHashGridVisualizationEnabled;
			uint3 _;
		} SHARC;
	} RTXGI;
	NRDSettings NRD;
};
ConstantBuffer<GraphicsSettings> g_graphicsSettings : register(b0);

ConstantBuffer<SceneData> g_sceneData : register(b1);

ConstantBuffer<Camera> g_camera : register(b2);

StructuredBuffer<InstanceData> g_instanceData : register(t1);
StructuredBuffer<ObjectData> g_objectData : register(t2);

RWTexture2D<float3> g_radiance : register(u0);
RWTexture2D<float> g_linearDepth : register(u1);
RWTexture2D<float> g_normalizedDepth : register(u2);
RWTexture2D<float3> g_motionVectors : register(u3);
RWTexture2D<float4> g_baseColorMetalness : register(u4);
RWTexture2D<float4> g_normals : register(u5);
RWTexture2D<float> g_roughness : register(u6);
RWTexture2D<float4> g_normalRoughness : register(u7);
RWTexture2D<float4> g_noisyDiffuse : register(u8);
RWTexture2D<float4> g_noisySpecular : register(u9);

#if ENABLE_SHARC
RWStructuredBuffer<uint64_t> g_sharcHashEntries : register(u10);
RWStructuredBuffer<uint4> g_sharcPreviousVoxelData : register(u11);
RWStructuredBuffer<uint4> g_sharcVoxelData : register(u12);
#endif

#include "RaytracingHelpers.hlsli"

float3 CalculateMotionVector(float2 UV, uint2 pixelDimensions, float linearDepth, HitInfo hitInfo)
{
	float3 previousPosition;
	if (g_sceneData.IsStatic)
	{
		previousPosition = hitInfo.Position;
	}
	else
	{
		previousPosition = hitInfo.ObjectPosition;
		const MeshResourceDescriptorIndices meshIndices = g_objectData[hitInfo.ObjectIndex].ResourceDescriptorIndices.Mesh;
		if (meshIndices.MotionVectors != ~0u)
		{
			const StructuredBuffer<float3> meshMotionVectors = ResourceDescriptorHeap[meshIndices.MotionVectors];
			const uint3 indices = MeshHelpers::Load3Indices(ResourceDescriptorHeap[meshIndices.Indices], hitInfo.PrimitiveIndex);
			const float3 motionVectors[] = { meshMotionVectors[indices[0]], meshMotionVectors[indices[1]], meshMotionVectors[indices[2]] };
			previousPosition += Vertex::Interpolate(motionVectors, hitInfo.Barycentrics);
		}
		previousPosition = Geometry::AffineTransform(g_instanceData[hitInfo.InstanceIndex].PreviousObjectToWorld, previousPosition);
	}
	return float3((Geometry::GetScreenUv(g_camera.PreviousWorldToProjection, previousPosition) - UV) * pixelDimensions, Geometry::AffineTransform(g_camera.PreviousWorldToView, previousPosition).z - linearDepth);
}

RaytracingShaderConfig ShaderConfig = { 0, 0 };
RaytracingPipelineConfig PipelineConfig = { 1 };
GlobalRootSignature GlobalRootSignature =
{
	"RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED),"
	"StaticSampler(s0),"
	"SRV(t0),"
	"CBV(b0),"
	"CBV(b1),"
	"CBV(b2),"
	"SRV(t1),"
	"SRV(t2),"
	"DescriptorTable(UAV(u0)),"
	"DescriptorTable(UAV(u1)),"
	"DescriptorTable(UAV(u2)),"
	"DescriptorTable(UAV(u3)),"
	"DescriptorTable(UAV(u4)),"
	"DescriptorTable(UAV(u5)),"
	"DescriptorTable(UAV(u6)),"
	"DescriptorTable(UAV(u7)),"
	"DescriptorTable(UAV(u8)),"
	"DescriptorTable(UAV(u9)),"
#if ENABLE_SHARC
	"UAV(u10),"
	"UAV(u11),"
	"UAV(u12),"
#endif
	"DescriptorTable(UAV(u1024))"
};
[shader("raygeneration")]
void RayGeneration()
{
	const uint2 pixelPosition = DispatchRaysIndex().xy, pixelDimensions = DispatchRaysDimensions().xy;

	Rng::Hash::Initialize(pixelPosition, g_graphicsSettings.FrameIndex);

	float3 primaryRadiance;
	float linearDepth = 1.#INF, normalizedDepth = !g_camera.IsNormalizedDepthReversed;
	float3 motionVector = 0;
	float4 normalRoughness = 0;

	HitInfo primarySurfaceHitInfo;

	BSDFSample primarySurfaceBSDFSample = (BSDFSample)0;
	bool primarySurfaceHasTexture = false;

	const float2 UV = Math::CalculateUV(pixelPosition, pixelDimensions,
#if SHARC_UPDATE
		Rng::Hash::GetFloat() - 0.5f
#else
		g_camera.Jitter
#endif
	);
	const RayDesc primaryRayDesc = g_camera.GeneratePinholeRay(Math::CalculateNDC(UV));
	const bool isPrimarySurfaceHit = CastRay(primaryRayDesc, primarySurfaceHitInfo);
	if (isPrimarySurfaceHit)
	{
		const Material material = GetMaterial(g_objectData[primarySurfaceHitInfo.ObjectIndex], primarySurfaceHitInfo.TextureCoordinate);
		primarySurfaceBSDFSample.Initialize(material);
		primarySurfaceHasTexture = material.HasTexture;

		primaryRadiance = material.GetEmission();
		const float4 projection = Geometry::ProjectiveTransform(g_camera.WorldToProjection, primarySurfaceHitInfo.Position);
		linearDepth = projection.w;
		normalizedDepth = projection.z / projection.w;
		motionVector = CalculateMotionVector(UV, pixelDimensions, linearDepth, primarySurfaceHitInfo);
		normalRoughness = NRD_FrontEnd_PackNormalAndRoughness(primarySurfaceHitInfo.Normal, primarySurfaceBSDFSample.Roughness, EstimateDiffuseProbability(primarySurfaceBSDFSample.Albedo, primarySurfaceBSDFSample.Rf0, max(primarySurfaceBSDFSample.Roughness, MinRoughness), abs(dot(primarySurfaceHitInfo.Normal, -primaryRayDesc.Direction))) == 0);

		g_baseColorMetalness[pixelPosition] = float4(material.BaseColor.rgb, material.Metallic);
		g_normals[pixelPosition] = float4(Packing::EncodeUnitVector(primarySurfaceHitInfo.Normal, true), Packing::EncodeUnitVector(primarySurfaceHitInfo.GeometricNormal, true));
		g_roughness[pixelPosition] = material.Roughness;
	}
	else
	{
		primaryRadiance = GetEnvironmentLightColor(g_sceneData, primaryRayDesc.Direction);
	}

#if !SHARC_UPDATE
	g_radiance[pixelPosition] = primaryRadiance;
	g_linearDepth[pixelPosition] = linearDepth;
	g_normalizedDepth[pixelPosition] = normalizedDepth;
	g_motionVectors[pixelPosition] = motionVector;
	g_normalRoughness[pixelPosition] = normalRoughness;

	float3 radiance = 0;
#endif

	const uint samplesPerPixel =
#if SHARC_UPDATE
		1;
#else
		g_graphicsSettings.SamplesPerPixel;
#endif

#if ENABLE_SHARC
	SharcParameters sharcParameters;
	sharcParameters.gridParameters.cameraPosition = g_camera.Position;
	sharcParameters.gridParameters.sceneScale = g_graphicsSettings.RTXGI.SHARC.SceneScale;
	sharcParameters.gridParameters.logarithmBase = SHARC_GRID_LOGARITHM_BASE;
	sharcParameters.gridParameters.levelBias = SHARC_GRID_LEVEL_BIAS;
	sharcParameters.hashMapData.capacity = g_graphicsSettings.RTXGI.SHARC.Capacity;
	sharcParameters.hashMapData.hashEntriesBuffer = g_sharcHashEntries;
	sharcParameters.voxelDataBuffer = g_sharcVoxelData;
	sharcParameters.voxelDataBufferPrev = g_sharcPreviousVoxelData;
	sharcParameters.enableAntiFireflyFilter = g_graphicsSettings.RTXGI.SHARC.IsAntiFileflyEnabled;
#endif

	bool isDiffuse = true;
	float hitDistance = 1.#INF;
	for (uint sampleIndex = 0; sampleIndex < samplesPerPixel; sampleIndex++)
	{
		RayDesc rayDesc = primaryRayDesc;
		bool isHit = isPrimarySurfaceHit, hasTexture = primarySurfaceHasTexture;
		HitInfo hitInfo = primarySurfaceHitInfo;

		float3 emission = primaryRadiance;
		BSDFSample BSDFSample = primarySurfaceBSDFSample;

		LobeType lobeType;
		float3 L, throughput = 1;

#if SHARC_UPDATE
		SharcState sharcState;
		SharcInit(sharcState);
#else
		float3 sampleRadiance = 0;
#if SHARC_QUERY
		float previousRoughness = 0;
#endif
#endif

		for (uint bounceIndex = 0; bounceIndex <= g_graphicsSettings.Bounces; bounceIndex++)
		{
#if SHARC_UPDATE
			throughput = 1;
#endif

			if (bounceIndex)
			{
				rayDesc.Origin = hitInfo.GetSafeWorldRayOrigin(L);
				rayDesc.Direction = L;
				rayDesc.TMin = 0;
				rayDesc.TMax = 1.#INF;
				isHit = CastRay(
					rayDesc,
					hitInfo
#if !SHARC_QUERY
					, g_graphicsSettings.IsShaderExecutionReorderingEnabled
					&& (lobeType == LobeType::DiffuseReflection || hasTexture)
#endif
				);
			}

			if (!sampleIndex && bounceIndex == 1)
			{
				isDiffuse = lobeType == LobeType::DiffuseReflection;
				hitDistance = hitInfo.Distance;
			}

			if (!isHit)
			{
				const float3 environmentLightColor = bounceIndex ? GetEnvironmentLightColor(g_sceneData, rayDesc.Direction) : primaryRadiance;

#if SHARC_UPDATE
				SharcUpdateMiss(sharcParameters, sharcState, environmentLightColor);
#endif

				if (!bounceIndex)
				{
					return;
				}

#if !SHARC_UPDATE
				sampleRadiance += throughput * environmentLightColor;
#endif

				break;
			}

#if ENABLE_SHARC
			SharcHitData sharcHitData;
			sharcHitData.positionWorld = hitInfo.Position;
			sharcHitData.normalWorld = hitInfo.GeometricNormal;
#if SHARC_QUERY
			const uint gridLevel = HashGridGetLevel(hitInfo.Position, sharcParameters.gridParameters);
			const float voxelSize = HashGridGetVoxelSize(gridLevel, sharcParameters.gridParameters);
			bool isValidHit = hitInfo.Distance > voxelSize * sqrt(3);

			previousRoughness = min(previousRoughness, 0.99f);
			const float
				alpha = previousRoughness * previousRoughness,
				footprint = hitInfo.Distance * sqrt(0.5f * alpha * alpha / (1 - alpha * alpha));
			isValidHit &= footprint > voxelSize;

			float3 sharcRadiance;
			if (isValidHit && SharcGetCachedRadiance(sharcParameters, sharcHitData, sharcRadiance, false))
			{
				if (g_graphicsSettings.RTXGI.SHARC.IsHashGridVisualizationEnabled)
				{
					g_radiance[pixelPosition] = HashGridDebugColoredHash(primarySurfaceHitInfo.Position, sharcParameters.gridParameters);

					if (g_graphicsSettings.NRD.Denoiser != NRDDenoiser::None)
					{
						g_noisyDiffuse[pixelPosition] = g_noisySpecular[pixelPosition] = 0;
					}

					return;
				}

				sampleRadiance += throughput * sharcRadiance;

				break;
			}
#endif
#endif

			if (bounceIndex)
			{
				const Material material = GetMaterial(g_objectData[hitInfo.ObjectIndex], hitInfo.TextureCoordinate);
				hasTexture = material.HasTexture;
				emission = bounceIndex == 1 && !g_graphicsSettings.IsSecondarySurfaceEmissionIncluded
					&& BSDFSample.Roughness > 0 && lobeType != LobeType::Transmission ?
					0 : material.GetEmission();
				BSDFSample.Initialize(material);
			}

#if SHARC_UPDATE
			BSDFSample.Roughness = max(BSDFSample.Roughness, g_graphicsSettings.RTXGI.SHARC.RoughnessThreshold);

			if (!SharcUpdateHit(sharcParameters, sharcState, sharcHitData, emission, Rng::Hash::GetFloat()))
			{
				break;
			}
#else
			sampleRadiance += throughput * emission;
#endif

			if (g_graphicsSettings.IsRussianRouletteEnabled && bounceIndex > 3)
			{
				const float probability = max(throughput.r, max(throughput.g, throughput.b));
				if (Rng::Hash::GetFloat() >= probability)
				{
					break;
				}
				throughput /= probability;
			}

			float PDF, weight;
			if (!BSDFSample.Sample(hitInfo, -rayDesc.Direction, lobeType, L, PDF, weight))
			{
				break;
			}

			const float3 currentThroughput = BSDFSample.Evaluate(lobeType, hitInfo.Normal, -rayDesc.Direction, L) * weight;
			if (all(currentThroughput == 0))
			{
				break;
			}
			throughput *= currentThroughput;

#if SHARC_UPDATE
			SharcSetThroughput(sharcState, throughput);
#else
			if (Color::Luminance(throughput) <= g_graphicsSettings.ThroughputThreshold)
			{
				break;
			}
#if SHARC_QUERY
			previousRoughness += lobeType == LobeType::DiffuseReflection ? 1 : BSDFSample.Roughness;
#endif
#endif
		}

#if !SHARC_UPDATE
		radiance += sampleRadiance;
#endif
	}

#if !SHARC_UPDATE
	radiance = NRD_IsValidRadiance(radiance) ? radiance / samplesPerPixel : 0;

	if (g_graphicsSettings.NRD.Denoiser == NRDDenoiser::None)
	{
		g_radiance[pixelPosition] = radiance;
	}
	else
	{
		const float4 radianceHitDistance = float4(max(radiance - primaryRadiance, 0), hitDistance);
		PackNoisySignals(
			g_graphicsSettings.NRD,
			primarySurfaceHitInfo.Normal, -primaryRayDesc.Direction, linearDepth,
			primarySurfaceBSDFSample,
			0, 0, 0,
			isDiffuse ? radianceHitDistance : 0, isDiffuse ? 0 : radianceHitDistance, false,
			g_noisyDiffuse[pixelPosition], g_noisySpecular[pixelPosition]
		);
	}
#endif
}
