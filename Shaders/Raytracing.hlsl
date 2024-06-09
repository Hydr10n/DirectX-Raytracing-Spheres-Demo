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
	const float2 UV = Math::CalculateUV(pixelPosition, pixelDimensions, g_camera.Jitter);
	const RayDesc rayDesc = g_camera.GeneratePinholeRay(Math::CalculateNDC(UV));
	const float3 V = -rayDesc.Direction;
	bool hit = CastRay(rayDesc, hitInfo, false);
	if (hit)
	{
		NoV = abs(dot(hitInfo.Normal, V));
		material = GetMaterial(hitInfo.ObjectIndex, hitInfo.TextureCoordinate);
		STL::BRDF::ConvertBaseColorMetalnessToAlbedoRf0(material.BaseColor.rgb, material.Metallic, albedo, Rf0);
		diffuseProbability = Material::EstimateDiffuseProbability(albedo, Rf0, material.Roughness, NoV);

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

	g_linearDepth[pixelPosition] = linearDepth;
	g_normalizedDepth[pixelPosition] = normalizedDepth;
	g_motionVectors[pixelPosition] = motionVector;
	g_baseColorMetalness[pixelPosition] = baseColorMetalness;
	g_emissiveColor[pixelPosition] = emissiveColor;
	g_normals[pixelPosition] = normals;
	g_roughness[pixelPosition] = roughness;
	g_normalRoughness[pixelPosition] = normalRoughness;

	float3 radiance = 0;
	float4 noisyDiffuse = float4(0, 0, 0, 1.#INF), noisySpecular = noisyDiffuse;

	if (hit)
	{
		bool isDiffuse = true;
		float hitDistance = 1.#INF;
		for (uint sampleIndex = 0; sampleIndex < g_graphicsSettings.SamplesPerPixel; sampleIndex++)
		{
			const ScatterResult scatterResult = material.Scatter(hitInfo, rayDesc.Direction);

			float3 sampleRadiance = 0, throughput = scatterResult.Throughput;
			float sampleHitDistance = 1.#INF;
			HitInfo sampleHitInfo = hitInfo;
			ScatterResult sampleScatterResult = scatterResult;
			if (STL::Color::Luminance(throughput) > g_graphicsSettings.ThroughputThreshold)
			{
				for (uint bounceIndex = 0; bounceIndex < g_graphicsSettings.Bounces; bounceIndex++)
				{
					const RayDesc rayDesc = { sampleHitInfo.GetSafeWorldRayOrigin(sampleScatterResult.Direction), 0, sampleScatterResult.Direction, 1.#INF };
					if (!CastRay(rayDesc, sampleHitInfo, g_graphicsSettings.IsShaderExecutionReorderingEnabled))
					{
						const float3 environmentLightColor = GetEnvironmentLightColor(rayDesc.Direction);
						
						sampleRadiance += throughput * environmentLightColor;

						break;
					}

					if (!bounceIndex)
					{
						sampleHitDistance = sampleHitInfo.Distance;
					}

					Material sampleMaterial = GetMaterial(sampleHitInfo.ObjectIndex, sampleHitInfo.TextureCoordinate);
					
					sampleRadiance += throughput * sampleMaterial.EmissiveColor;

					if (g_graphicsSettings.IsRussianRouletteEnabled && bounceIndex > 2)
					{
						const float probability = max(throughput.r, max(throughput.g, throughput.b));
						if (STL::Rng::Hash::GetFloat() >= probability)
						{
							break;
						}
						throughput /= probability;
					}

					sampleScatterResult = sampleMaterial.Scatter(sampleHitInfo, rayDesc.Direction);

					throughput *= sampleScatterResult.Throughput;

					if (STL::Color::Luminance(throughput) <= g_graphicsSettings.ThroughputThreshold)
					{
						break;
					}
				}
			}

			radiance += sampleRadiance;

			if (!sampleIndex)
			{
				isDiffuse = scatterResult.Type == ScatterType::DiffuseReflection;
				hitDistance = sampleHitDistance;
			}
		}
		
		radiance = NRD_IsValidRadiance(radiance) ? radiance / g_graphicsSettings.SamplesPerPixel : 0;

		if (g_graphicsSettings.NRD.Denoiser != NRDDenoiser::None)
		{
			const float4 radianceHitDistance = float4(radiance, hitDistance);
			PackNoisySignals(
				g_graphicsSettings.NRD,
				NoV, linearDepth,
				albedo, Rf0, material.Roughness,
				0, 0, 1.#INF,
				isDiffuse ? radianceHitDistance : 0, isDiffuse ? 0 : radianceHitDistance, false,
				noisyDiffuse, noisySpecular
			);
		}
		
		radiance += material.EmissiveColor;
	}
	else if (!GetEnvironmentColor(rayDesc.Direction, radiance))
	{
		radiance = GetEnvironmentLightColor(rayDesc.Direction);
	}

	g_color[pixelPosition] = radiance;
	g_noisyDiffuse[pixelPosition] = noisyDiffuse;
	g_noisySpecular[pixelPosition] = noisySpecular;
}
