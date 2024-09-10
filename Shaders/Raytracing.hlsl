#include "Raytracing.hlsli"

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
	"DescriptorTable(UAV(u10)),"
#if ENABLE_SHARC
	"UAV(u11),"
	"UAV(u12),"
	"UAV(u13),"
#endif
	"DescriptorTable(UAV(u1024))"
};
[shader("raygeneration")]
void RayGeneration()
{
	const uint2 pixelPosition = DispatchRaysIndex().xy, pixelDimensions = DispatchRaysDimensions().xy;

	Rng::Hash::Initialize(pixelPosition, g_graphicsSettings.FrameIndex);

	float linearDepth = 1.#INF, normalizedDepth = !g_camera.IsNormalizedDepthReversed;
	float3 motionVector = 0;
	float4 normals, normalRoughness = 0;

	HitInfo primarySurfaceHitInfo;
	float NoV;

	Material primarySurfaceMaterial = (Material)0;
	BRDFSample primarySurfaceBRDFSample;

	const float2 UV = Math::CalculateUV(pixelPosition, pixelDimensions,
#if SHARC_UPDATE
		Rng::Hash::GetFloat()
#else
		g_camera.Jitter
#endif
	);
	const RayDesc primaryRayDesc = g_camera.GeneratePinholeRay(Math::CalculateNDC(UV));
	const bool isPrimarySurfaceHit = CastRay(primaryRayDesc, primarySurfaceHitInfo);
	if (isPrimarySurfaceHit)
	{
		NoV = abs(dot(primarySurfaceHitInfo.Normal, -primaryRayDesc.Direction));

		primarySurfaceMaterial = GetMaterial(primarySurfaceHitInfo.ObjectIndex, primarySurfaceHitInfo.TextureCoordinate);
		primarySurfaceBRDFSample.Initialize(primarySurfaceMaterial);

		linearDepth = dot(primarySurfaceHitInfo.Position - g_camera.Position, normalize(g_camera.ForwardDirection));
		const float4 projection = Geometry::ProjectiveTransform(g_camera.WorldToProjection, primarySurfaceHitInfo.Position);
		normalizedDepth = projection.z / projection.w;
		motionVector = CalculateMotionVector(UV, pixelDimensions, linearDepth, primarySurfaceHitInfo);
		normals = float4(Packing::EncodeUnitVector(primarySurfaceHitInfo.Normal, true), Packing::EncodeUnitVector(primarySurfaceHitInfo.GeometricNormal, true));
		normalRoughness = NRD_FrontEnd_PackNormalAndRoughness(primarySurfaceHitInfo.Normal, primarySurfaceMaterial.Roughness, EstimateDiffuseProbability(primarySurfaceBRDFSample.Albedo, primarySurfaceBRDFSample.Rf0, max(primarySurfaceBRDFSample.Roughness, MinRoughness), NoV) == 0);
	}

#if !SHARC_UPDATE
	g_linearDepth[pixelPosition] = linearDepth;
	g_normalizedDepth[pixelPosition] = normalizedDepth;
	g_motionVectors[pixelPosition] = motionVector;
	g_baseColorMetalness[pixelPosition] = float4(primarySurfaceMaterial.BaseColor.rgb, primarySurfaceMaterial.Metallic);
	g_emissiveColor[pixelPosition] = primarySurfaceMaterial.EmissiveColor;
	g_normals[pixelPosition] = normals;
	g_roughness[pixelPosition] = primarySurfaceMaterial.Roughness;
	g_normalRoughness[pixelPosition] = normalRoughness;
#endif

	float3 radiance = 0;
	float4 noisyDiffuse = float4(0, 0, 0, 1.#INF), noisySpecular = noisyDiffuse;

	const uint samplesPerPixel =
#if SHARC_UPDATE
		1;
#else
		g_graphicsSettings.SamplesPerPixel;
#endif

#if ENABLE_SHARC
	SharcState sharcState;
	sharcState.gridParameters.cameraPosition = g_camera.Position;
	sharcState.gridParameters.logarithmBase = SHARC_GRID_LOGARITHM_BASE;
	sharcState.gridParameters.sceneScale = g_graphicsSettings.RTXGI.SHARC.SceneScale;
	sharcState.hashMapData.capacity = g_graphicsSettings.RTXGI.SHARC.Capacity;
	sharcState.hashMapData.hashEntriesBuffer = g_sharcHashEntries;
	sharcState.voxelDataBuffer = g_sharcVoxelData;
#if SHARC_ENABLE_CACHE_RESAMPLING
	sharcState.voxelDataBufferPrev = g_sharcPreviousVoxelData;
#endif
#endif

	bool isDiffuse = true;
	float hitDistance = 1.#INF;
	for (uint sampleIndex = 0; sampleIndex < samplesPerPixel; sampleIndex++)
	{
		RayDesc rayDesc = primaryRayDesc;
		bool isHit = isPrimarySurfaceHit, hasTexture = primarySurfaceMaterial.HasTexture;
		HitInfo hitInfo = primarySurfaceHitInfo;

		float3 emissiveColor = primarySurfaceMaterial.EmissiveColor;
		BSDFSample BSDFSample;
		BSDFSample.Initialize(primarySurfaceMaterial);

		LobeType lobeType;
		float3 L, sampleRadiance = 0, throughput = 1;

#if SHARC_UPDATE
		SharcInit(sharcState);
#elif SHARC_QUERY
		float previousRoughness = 0;
#endif

		for (uint bounceIndex = 0; bounceIndex <= g_graphicsSettings.Bounces; bounceIndex++)
		{
#if SHARC_UPDATE
			sampleRadiance = 0;
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
					, g_graphicsSettings.IsShaderExecutionReorderingEnabled && (lobeType == LobeType::DiffuseReflection || hasTexture)
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
				float3 environmentColor = 0, environmentLightColor = 0;
				if (bounceIndex || !GetEnvironmentColor(rayDesc.Direction, environmentColor))
				{
					environmentLightColor = GetEnvironmentLightColor(rayDesc.Direction);
				}
				environmentColor += environmentLightColor;

				if (!bounceIndex)
				{
					environmentColor *= samplesPerPixel;

					sampleIndex = samplesPerPixel;
				}

				sampleRadiance += throughput * environmentColor;

#if SHARC_UPDATE
				SharcUpdateMiss(sharcState, environmentLightColor);
#endif

				break;
			}

#if ENABLE_SHARC
			SharcHitData sharcHitData;
			sharcHitData.positionWorld = hitInfo.Position;
			sharcHitData.normalWorld = hitInfo.GeometricNormal;
#endif

#if SHARC_QUERY
			if (g_graphicsSettings.RTXGI.SHARC.IsHashGridVisualizationEnabled)
			{
				SharcGetCachedRadiance(sharcState, sharcHitData, sampleRadiance, true);

				break;
			}

			const uint gridLevel = GetGridLevel(hitInfo.Position, sharcState.gridParameters);
			const float voxelSize = GetVoxelSize(gridLevel, sharcState.gridParameters);
			bool isValidHit = hitInfo.Distance > voxelSize * sqrt(3);

			previousRoughness = min(previousRoughness, 0.99f);
			const float
				alpha = previousRoughness * previousRoughness,
				footprint = hitInfo.Distance * sqrt(0.5f * alpha * alpha / (1 - alpha * alpha));
			isValidHit &= footprint > voxelSize;

			float3 sharcRadiance;
			if (isValidHit && SharcGetCachedRadiance(sharcState, sharcHitData, sharcRadiance, false))
			{
				sampleRadiance += throughput * sharcRadiance;

				break;
			}
#endif

			if (bounceIndex)
			{
				const Material material = GetMaterial(hitInfo.ObjectIndex, hitInfo.TextureCoordinate);
				hasTexture = material.HasTexture;
				emissiveColor = material.EmissiveColor;
				BSDFSample.Initialize(material);
			}

			sampleRadiance += throughput * emissiveColor;

#if SHARC_UPDATE
			BSDFSample.Roughness = max(BSDFSample.Roughness, g_graphicsSettings.RTXGI.SHARC.RoughnessThreshold);

			if (!SharcUpdateHit(sharcState, sharcHitData, sampleRadiance, Rng::Hash::GetFloat()))
			{
				break;
			}
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

		radiance += sampleRadiance;
	}

#if !SHARC_UPDATE
	radiance = NRD_IsValidRadiance(radiance) ? radiance / samplesPerPixel : 0;
	g_color[pixelPosition] = radiance;

	if (g_graphicsSettings.NRD.Denoiser != NRDDenoiser::None)
	{
		if (isPrimarySurfaceHit)
		{
			const float4 radianceHitDistance = float4(max(radiance - primarySurfaceMaterial.EmissiveColor, 0), hitDistance);
			PackNoisySignals(
				g_graphicsSettings.NRD,
				NoV, linearDepth,
				primarySurfaceBRDFSample,
				0, 0, 1.#INF,
				isDiffuse ? radianceHitDistance : 0, isDiffuse ? 0 : radianceHitDistance, false,
				noisyDiffuse, noisySpecular
			);
		}
		g_noisyDiffuse[pixelPosition] = noisyDiffuse;
		g_noisySpecular[pixelPosition] = noisySpecular;
	}
#endif
}
