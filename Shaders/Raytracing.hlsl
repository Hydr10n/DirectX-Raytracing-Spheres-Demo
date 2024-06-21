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

	STL::Rng::Hash::Initialize(pixelPosition, g_graphicsSettings.FrameIndex);

	float linearDepth = 1.#INF, normalizedDepth = !g_camera.IsNormalizedDepthReversed;
	float3 motionVector = 0;
	float4 baseColorMetalness = 0;
	float3 emissiveColor = 0;
	float4 normals = 0;
	float roughness = 0;
	float4 normalRoughness = 0;

	HitInfo hitInfo;
	float NoV;
	Material material;
	float3 albedo, Rf0;
	float diffuseProbability;
	const float2 UV = Math::CalculateUV(pixelPosition, pixelDimensions,
#if SHARC_UPDATE
		STL::Rng::Hash::GetFloat()
#else
		g_camera.Jitter
#endif
	);
	const RayDesc rayDesc = g_camera.GeneratePinholeRay(Math::CalculateNDC(UV));
	const float3 V = -rayDesc.Direction;
	const bool isHit = CastRay(rayDesc, hitInfo, false);
	if (isHit)
	{
		NoV = abs(dot(hitInfo.Normal, V));
		material = GetMaterial(hitInfo.ObjectIndex, hitInfo.TextureCoordinate);
		STL::BRDF::ConvertBaseColorMetalnessToAlbedoRf0(material.BaseColor.rgb, material.Metallic, albedo, Rf0);
		diffuseProbability = Material::EstimateDiffuseProbability(albedo, Rf0, max(material.Roughness, MinRoughness), NoV);

		linearDepth = dot(hitInfo.Position - g_camera.Position, normalize(g_camera.ForwardDirection));
		const float4 projection = STL::Geometry::ProjectiveTransform(g_camera.WorldToProjection, hitInfo.Position);
		normalizedDepth = projection.z / projection.w;
		motionVector = CalculateMotionVector(UV, pixelDimensions, linearDepth, hitInfo);
		baseColorMetalness = float4(material.BaseColor.rgb, material.Metallic);
		emissiveColor = material.EmissiveColor;
		normals = float4(STL::Packing::EncodeUnitVector(hitInfo.Normal, true), STL::Packing::EncodeUnitVector(hitInfo.GeometricNormal, true));
		roughness = material.Roughness;
		normalRoughness = NRD_FrontEnd_PackNormalAndRoughness(hitInfo.Normal, material.Roughness, diffuseProbability == 0);
	}

#if !SHARC_UPDATE
	g_linearDepth[pixelPosition] = linearDepth;
	g_normalizedDepth[pixelPosition] = normalizedDepth;
	g_motionVectors[pixelPosition] = motionVector;
	g_baseColorMetalness[pixelPosition] = baseColorMetalness;
	g_emissiveColor[pixelPosition] = emissiveColor;
	g_normals[pixelPosition] = normals;
	g_roughness[pixelPosition] = roughness;
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
		RayDesc sampleRayDesc = rayDesc;
		bool sampleIsHit = isHit;
		HitInfo sampleHitInfo = hitInfo;
		Material sampleMaterial = material;
		float3 sampleRadiance = 0, throughput = 1;
		ScatterResult scatterResult;

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
				sampleRayDesc.Origin = sampleHitInfo.GetSafeWorldRayOrigin(scatterResult.Direction);
				sampleRayDesc.Direction = scatterResult.Direction;
				sampleRayDesc.TMin = 0;
				sampleRayDesc.TMax = 1.#INF;
				sampleIsHit = CastRay(sampleRayDesc, sampleHitInfo, g_graphicsSettings.IsShaderExecutionReorderingEnabled);
			}

			if (!sampleIndex && bounceIndex == 1)
			{
				isDiffuse = scatterResult.Type == ScatterType::DiffuseReflection;

				if (sampleIsHit)
				{
					hitDistance = sampleHitInfo.Distance;
				}
			}

			if (!sampleIsHit)
			{
				if (!bounceIndex)
				{
					sampleIndex = samplesPerPixel;
				}

				float3 environmentColor = 0, environmentLightColor = 0;
				if (bounceIndex || !GetEnvironmentColor(sampleRayDesc.Direction, environmentColor))
				{
					environmentLightColor = GetEnvironmentLightColor(sampleRayDesc.Direction);
				}

				sampleRadiance += throughput * (environmentColor + environmentLightColor);

#if SHARC_UPDATE
				SharcUpdateMiss(sharcState, environmentLightColor);
#endif

				break;
			}

			if (bounceIndex)
			{
				sampleMaterial = GetMaterial(sampleHitInfo.ObjectIndex, sampleHitInfo.TextureCoordinate);
			}

#if ENABLE_SHARC
			SharcHitData sharcHitData;
			sharcHitData.positionWorld = sampleHitInfo.Position;
			sharcHitData.normalWorld = sampleHitInfo.GeometricNormal;
#endif

#if SHARC_QUERY
			if (g_graphicsSettings.RTXGI.SHARC.IsHashGridVisualizationEnabled)
			{
				SharcGetCachedRadiance(sharcState, sharcHitData, sampleRadiance, true);

				break;
			}

			const uint gridLevel = GetGridLevel(sampleHitInfo.Position, sharcState.gridParameters);
			const float voxelSize = GetVoxelSize(gridLevel, sharcState.gridParameters);
			bool isValidHit = sampleHitInfo.Distance > voxelSize * lerp(1, 2, STL::Rng::Hash::GetFloat());

			previousRoughness = min(previousRoughness, 0.99f);
			const float
				alpha = previousRoughness * previousRoughness,
				footprint = sampleHitInfo.Distance * sqrt(0.5f * alpha * alpha / (1 - alpha * alpha));
			isValidHit &= footprint * lerp(1, 1.5f, STL::Rng::Hash::GetFloat()) > voxelSize;

			float3 sharcRadiance;
			if (isValidHit && SharcGetCachedRadiance(sharcState, sharcHitData, sharcRadiance, false))
			{
				sampleRadiance += throughput * sharcRadiance;

				break;
			}
#endif

			sampleRadiance += sampleMaterial.EmissiveColor * throughput;

#if SHARC_UPDATE
			sampleMaterial.Roughness = max(sampleMaterial.Roughness, g_graphicsSettings.RTXGI.SHARC.RoughnessThreshold);

			if (!SharcUpdateHit(sharcState, sharcHitData, sampleRadiance, STL::Rng::Hash::GetFloat()))
			{
				break;
			}
#endif

			if (g_graphicsSettings.IsRussianRouletteEnabled && bounceIndex > 3)
			{
				const float probability = max(throughput.r, max(throughput.g, throughput.b));
				if (STL::Rng::Hash::GetFloat() >= probability)
				{
					break;
				}
				throughput /= probability;
			}

			scatterResult = sampleMaterial.Scatter(sampleHitInfo, sampleRayDesc.Direction);

			if (all(scatterResult.Throughput == 0))
			{
				break;
			}

			throughput *= scatterResult.Throughput;

#if SHARC_UPDATE
			SharcSetThroughput(sharcState, throughput);
#else
			if (STL::Color::Luminance(throughput) <= g_graphicsSettings.ThroughputThreshold)
			{
				break;
			}
#if SHARC_QUERY
			previousRoughness += scatterResult.Type == ScatterType::DiffuseReflection ? 1 : sampleMaterial.Roughness;
#endif
#endif
		}

		radiance += sampleRadiance;
	}

#if !SHARC_UPDATE
	radiance = NRD_IsValidRadiance(radiance) ? radiance / samplesPerPixel : 0;

	if (isHit && g_graphicsSettings.NRD.Denoiser != NRDDenoiser::None)
	{
		const float4 radianceHitDistance = float4(max(radiance - material.EmissiveColor, 0), hitDistance);
		PackNoisySignals(
			g_graphicsSettings.NRD,
			NoV, linearDepth,
			albedo, Rf0, material.Roughness,
			0, 0, 1.#INF,
			isDiffuse ? radianceHitDistance : 0, isDiffuse ? 0 : radianceHitDistance, false,
			noisyDiffuse, noisySpecular
		);
	}

	g_color[pixelPosition] = radiance;
	g_noisyDiffuse[pixelPosition] = noisyDiffuse;
	g_noisySpecular[pixelPosition] = noisySpecular;
#endif
}
