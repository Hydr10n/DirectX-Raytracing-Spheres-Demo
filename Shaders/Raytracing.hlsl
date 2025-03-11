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
	uint2 RenderSize;
	uint FrameIndex, Bounces, SamplesPerPixel;
	float ThroughputThreshold;
	bool IsRussianRouletteEnabled, IsShaderExecutionReorderingEnabled, IsDIEnabled;
	Denoiser Denoiser;
	uint2 _;
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
};
ConstantBuffer<GraphicsSettings> g_graphicsSettings : register(b0);

ConstantBuffer<SceneData> g_sceneData : register(b1);

ConstantBuffer<Camera> g_camera : register(b2);

StructuredBuffer<ObjectData> g_objectData : register(t1);

Texture2D<float4> g_position : register(t2);
Texture2D<float2> g_flatNormal : register(t3);
Texture2D<float2> g_geometricNormal : register(t4);
Texture2D<float> g_linearDepth : register(t5);
Texture2D<float4> g_baseColorMetalness : register(t6);
Texture2D<float4> g_normalRoughness : register(t7);
Texture2D<float> g_IOR : register(t8);
Texture2D<float> g_transmission : register(t9);
#if SHARC_UPDATE
Texture2D<float3> g_radiance : register(t10);
#else
RWTexture2D<float3> g_radiance : register(u0);
#endif
RWTexture2D<float4> g_diffuse : register(u1);
RWTexture2D<float4> g_specular : register(u2);
RWTexture2D<float> g_specularHitDistance : register(u3);

#if ENABLE_SHARC
RWStructuredBuffer<uint64_t> g_sharcHashEntries : register(u4);
RWStructuredBuffer<uint4> g_sharcPreviousVoxelData : register(u5);
RWStructuredBuffer<uint4> g_sharcVoxelData : register(u6);
#endif

#include "RaytracingHelpers.hlsli"

#if SHARC_UPDATE
#define LOAD(Texture) g_##Texture[clamp(uint2(UV * g_graphicsSettings.RenderSize), 0, g_graphicsSettings.RenderSize - 1)]
#else
#define LOAD(Texture) g_##Texture[pixelPosition]
#endif

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
	"DescriptorTable(SRV(t2)),"
	"DescriptorTable(SRV(t3)),"
	"DescriptorTable(SRV(t4)),"
	"DescriptorTable(SRV(t5)),"
	"DescriptorTable(SRV(t6)),"
	"DescriptorTable(SRV(t7)),"
	"DescriptorTable(SRV(t8)),"
	"DescriptorTable(SRV(t9)),"
#if SHARC_UPDATE
	"DescriptorTable(SRV(t10)),"
#else
	"DescriptorTable(UAV(u0)),"
#endif
	"DescriptorTable(UAV(u1)),"
	"DescriptorTable(UAV(u2)),"
	"DescriptorTable(UAV(u3)),"
#if ENABLE_SHARC
	"UAV(u4),"
	"UAV(u5),"
	"UAV(u6),"
#endif
	"DescriptorTable(UAV(u1024))"
};
[shader("raygeneration")]
void RayGeneration()
{
	const uint2 pixelPosition = DispatchRaysIndex().xy, pixelDimensions = DispatchRaysDimensions().xy;

	Rng::Hash::Initialize(pixelPosition, g_graphicsSettings.FrameIndex);

	const float2 UV = Math::CalculateUV(pixelPosition, pixelDimensions,
#if SHARC_UPDATE
		Rng::Hash::GetFloat() - 0.5f
#else
		g_camera.Jitter
#endif
	);

	const float linearDepth = LOAD(linearDepth);
	const float3 primaryRadiance = LOAD(radiance);

	HitInfo primarySurfaceHitInfo;
	BSDFSample primarySurfaceBSDFSample;
	float3 directDiffuse = 0, directSpecular = 0, DI = 0;
	float lightDistance = 0;
	bool isDIValid = false;
	const RayDesc primaryRayDesc = g_camera.GeneratePinholeRay(Math::CalculateNDC(UV));
	const bool isPrimarySurfaceHit = isfinite(linearDepth);
	if (isPrimarySurfaceHit)
	{
		const float4 position = LOAD(position);
		const float4 normalRoughness = LOAD(normalRoughness);
		primarySurfaceHitInfo.Initialize(
			position.xyz, position.w,
			Packing::DecodeUnitVector(LOAD(flatNormal), true),
			Packing::DecodeUnitVector(LOAD(geometricNormal), true),
			normalRoughness.xyz,
			primaryRayDesc.Direction
		);
		primarySurfaceHitInfo.Distance = length(position.xyz - g_camera.Position);

		const float4 baseColorMetalness = LOAD(baseColorMetalness);
		primarySurfaceBSDFSample.Initialize(
			baseColorMetalness.rgb,
			baseColorMetalness.a,
			normalRoughness.w,
			LOAD(IOR),
			baseColorMetalness.a < 1 ? LOAD(transmission) : 0,
			primarySurfaceHitInfo.IsFrontFace
		);

		if (g_graphicsSettings.IsDIEnabled)
		{
			directDiffuse = LOAD(diffuse).rgb;
			directSpecular = LOAD(specular).rgb;

#if SHARC_QUERY
			if (g_graphicsSettings.Denoiser == Denoiser::None
				|| g_graphicsSettings.Denoiser == Denoiser::DLSSRayReconstruction)
#endif
			{
				DI = directDiffuse + directSpecular;
				isDIValid = any(DI > 0);
			}
		}
	}

	float3 radiance = 0;

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
	bool hasSampledTexture = false;
	for (uint sampleIndex = 0; sampleIndex < samplesPerPixel; sampleIndex++)
	{
		RayDesc rayDesc = primaryRayDesc;
		bool isHit = isPrimarySurfaceHit;
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
					&& (lobeType == LobeType::DiffuseReflection || hasSampledTexture)
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
			sharcHitData.normalWorld = hitInfo.GetFrontFlatNormal();
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
					g_radiance[pixelPosition] += HashGridDebugColoredHash(primarySurfaceHitInfo.Position, sharcParameters.gridParameters);

					return;
				}

				sampleRadiance += throughput * sharcRadiance;

				break;
			}
#endif
#endif

			if (bounceIndex)
			{
				const ObjectData objectData = g_objectData[hitInfo.ObjectIndex];
				const Material material = EvaluateMaterial(
					hitInfo.ShadingNormal, hitInfo.GetFrontTangent(),
					objectData.Material,
					objectData.TextureMapInfoArray, hitInfo.TextureCoordinates,
					hasSampledTexture
				);
				emission = isDIValid && bounceIndex == 1 ? 0 : material.GetEmission();
				BSDFSample.Initialize(material, hitInfo.IsFrontFace);
			}

#if SHARC_UPDATE
			BSDFSample.Roughness = max(BSDFSample.Roughness, g_graphicsSettings.RTXGI.SHARC.RoughnessThreshold);

			if (!SharcUpdateHit(
				sharcParameters, sharcState, sharcHitData,
				(isDIValid && !bounceIndex ? DI : 0) + emission,
				Rng::Hash::GetFloat()
			))
			{
				break;
			}
#elif SHARC_QUERY
			sampleRadiance += (isDIValid && !bounceIndex ? DI : 0) + throughput * emission;
#else
			sampleRadiance += throughput * emission;
#endif

			SurfaceVectors surfaceVectors;
			surfaceVectors.Initialize(hitInfo.IsFrontFace, hitInfo.GeometricNormal, hitInfo.ShadingNormal);

			const float3 V = -rayDesc.Direction;

			LobeWeightArray lobeWeights;
			BSDFSample.ComputeLobeWeights(surfaceVectors, V, lobeWeights);
			if (!BSDFSample.Sample(surfaceVectors, V, lobeWeights, Rng::Hash::GetFloat4(), L, lobeType))
			{
				break;
			}

			const float PDF = BSDFSample.EvaluatePDF(surfaceVectors, L, V, lobeWeights, lobeType);
			if (PDF == 0)
			{
				break;
			}

			const float3 currentThroughput = BSDFSample.Evaluate(surfaceVectors, L, V, lobeWeights, lobeType);
			if (all(currentThroughput == 0))
			{
				break;
			}
			throughput *= currentThroughput / PDF;

			if (g_graphicsSettings.IsRussianRouletteEnabled && bounceIndex > 3)
			{
				const float probability = max(throughput.r, max(throughput.g, throughput.b));
				if (Rng::Hash::GetFloat() >= probability)
				{
					break;
				}
				throughput /= probability;
			}

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
	radiance = all(isfinite(radiance)) ? radiance / samplesPerPixel : 0;

	if (g_graphicsSettings.Denoiser == Denoiser::None)
	{
#if !SHARC_QUERY
		radiance += DI;
#endif

		g_radiance[pixelPosition] = radiance;
	}
	else if (g_graphicsSettings.Denoiser == Denoiser::DLSSRayReconstruction)
	{
#if !SHARC_QUERY
		radiance += DI;
#endif

		g_radiance[pixelPosition] = radiance;

		if (!isDiffuse && isfinite(hitDistance))
		{
			g_specularHitDistance[pixelPosition] = hitDistance;
		}
	}
	else if (g_graphicsSettings.Denoiser == Denoiser::NRDReBLUR
		|| g_graphicsSettings.Denoiser == Denoiser::NRDReLAX)
	{
#if SHARC_QUERY
		directDiffuse = directSpecular = 0;
#endif

		const float4
			radianceHitDistance = float4(max(radiance - primaryRadiance, 0), hitDistance),
			indirectDiffuse = isDiffuse ? radianceHitDistance : 0,
			indirectSpecular = isDiffuse ? 0 : radianceHitDistance;
		g_diffuse[pixelPosition] = float4(directDiffuse + indirectDiffuse.rgb, indirectDiffuse.a);
		g_specular[pixelPosition] = float4(directSpecular + indirectSpecular.rgb, indirectSpecular.a);
	}
#endif
}
