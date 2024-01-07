#include "IndirectRay.hlsli"

#include "RTXDIAppBridge.hlsli"
#include "rtxdi/DIResamplingFunctions.hlsli"
#include "rtxdi/InitialSamplingFunctions.hlsli"

[RootSignature(
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
	"DescriptorTable(SRV(t3)),"
	"DescriptorTable(SRV(t4)),"
	"DescriptorTable(SRV(t5)),"
	"DescriptorTable(SRV(t6)),"
	"SRV(t7),"
	"SRV(t8),"
	"DescriptorTable(SRV(t9)),"
	"UAV(u10)"
)]
[numthreads(16, 16, 1)]
void main(uint2 pixelPosition : SV_DispatchThreadID) {
	if (any(pixelPosition >= g_graphicsSettings.RenderSize)) return;

	STL::Rng::Hash::Initialize(pixelPosition, g_graphicsSettings.FrameIndex);

	float linearDepth = 1.#INFf, normalizedDepth = !g_camera.IsNormalizedDepthReversed;
	float3 motionVector = 0;
	float4 baseColorMetalness = 0;
	float3 emissiveColor = 0;
	float4 normalRoughness = 0, geometricNormal = 0;

	HitInfo hitInfo;
	float NoV;
	Material material;
	float3 albedo, Rf0;
	float diffuseProbability;
	const float2 UV = Math::CalculateUV(pixelPosition, g_graphicsSettings.RenderSize, g_camera.Jitter);
	const RayDesc rayDesc = g_camera.GeneratePinholeRay(Math::CalculateNDC(UV));
	const float3 V = -rayDesc.Direction;
	const bool hit = CastRay(rayDesc, hitInfo);
	if (hit) {
		NoV = abs(dot(hitInfo.Normal, V));
		material = GetMaterial(hitInfo.ObjectIndex, hitInfo.TextureCoordinate);
		STL::BRDF::ConvertBaseColorMetalnessToAlbedoRf0(material.BaseColor.rgb, material.Metallic, albedo, Rf0);
		diffuseProbability = Material::EstimateDiffuseProbability(albedo, Rf0, material.Roughness, NoV);

		linearDepth = dot(hitInfo.Position - g_camera.Position, normalize(g_camera.ForwardDirection));
		const float4 projection = STL::Geometry::ProjectiveTransform(g_camera.WorldToProjection, hitInfo.Position);
		normalizedDepth = projection.z / projection.w;
		motionVector = CalculateMotionVector(UV, linearDepth, hitInfo);
		baseColorMetalness = float4(material.BaseColor.rgb, material.Metallic);
		emissiveColor = material.EmissiveColor;
		normalRoughness = NRD_FrontEnd_PackNormalAndRoughness(hitInfo.Normal, material.Roughness, diffuseProbability == 0);
		geometricNormal = NRD_FrontEnd_PackNormalAndRoughness(hitInfo.GeometricNormal, 0, 0);
	}

	g_linearDepth[pixelPosition] = linearDepth;
	g_normalizedDepth[pixelPosition] = normalizedDepth;
	g_motionVectors[pixelPosition] = motionVector;
	g_baseColorMetalness[pixelPosition] = baseColorMetalness;
	g_emissiveColor[pixelPosition] = emissiveColor;
	g_normalRoughness[pixelPosition] = normalRoughness;
	g_geometricNormals[pixelPosition] = geometricNormal;

	float3 radiance = 0;
	float4 noisyDiffuse = 0, noisySpecular = 0;

	const RTXDISettings RTXDISettings = g_graphicsSettings.RTXDI;
	RTXDI_DIReservoir reservoir = RTXDI_EmptyDIReservoir();

	if (hit) {
		bool isDiffuse = true;
		float hitDistance = 1.#INFf;
		const float bayer4x4 = STL::Sequence::Bayer4x4(pixelPosition, g_graphicsSettings.FrameIndex);
		for (uint i = 0; i < g_graphicsSettings.SamplesPerPixel; i++) {
			const ScatterResult scatterResult = material.Scatter(hitInfo, rayDesc.Direction, bayer4x4 + STL::Rng::Hash::GetFloat() / 16);
			const IndirectRay::TraceResult traceResult = IndirectRay::Trace(hitInfo, scatterResult.Direction);
			if (!i) {
				isDiffuse = scatterResult.Type == ScatterType::DiffuseReflection;
				hitDistance = traceResult.HitDistance;
			}
			radiance += traceResult.Radiance * scatterResult.Throughput;
		}

		radiance *= NRD_IsValidRadiance(radiance) ? 1.0f / g_graphicsSettings.SamplesPerPixel : 0;

		float3 directDiffuse = 0, directSpecular = 0;
		if (RTXDISettings.IsEnabled) {
			RAB_Surface surface;
			surface.Position = hitInfo.Position;
			surface.Normal = hitInfo.Normal;
			surface.GeometricNormal = hitInfo.GeometricNormal;
			surface.Albedo = albedo;
			surface.Rf0 = Rf0;
			surface.Roughness = material.Roughness;
			surface.DiffuseProbability = diffuseProbability;
			surface.ViewDirection = V;
			surface.LinearDepth = linearDepth;

			RAB_RandomSamplerState rng = RAB_InitRandomSampler(pixelPosition, 1);

			RAB_LightSample lightSample = RAB_EmptyLightSample(), BRDFSample = RAB_EmptyLightSample();
			const RTXDI_SampleParameters sampleParameters = RTXDI_InitSampleParameters(RTXDISettings.LocalLightSamples, 0, 0, RTXDISettings.BRDFSamples);
			const RTXDI_DIReservoir
				localReservoir = RTXDI_SampleLocalLights(rng, rng, surface, sampleParameters, ReSTIRDI_LocalLightSamplingMode_UNIFORM, RTXDISettings.LightBufferParameters.localLightBufferRegion, lightSample),
				BRDFReservoir = RTXDI_SampleBrdf(rng, surface, sampleParameters, RTXDISettings.LightBufferParameters, BRDFSample);
			RTXDI_CombineDIReservoirs(reservoir, localReservoir, 0.5f, localReservoir.targetPdf);
			const bool BRDFSelected = RTXDI_CombineDIReservoirs(reservoir, BRDFReservoir, RAB_GetNextRandom(rng), BRDFReservoir.targetPdf);
			if (BRDFSelected) lightSample = BRDFSample;

			RTXDI_FinalizeResampling(reservoir, 1, 1);
			reservoir.M = 1;

			if (RTXDI_IsValidDIReservoir(reservoir) && !BRDFSelected && !RAB_GetConservativeVisibility(surface, lightSample)) RTXDI_StoreVisibilityInDIReservoir(reservoir, 0, true);

			RTXDI_DISpatioTemporalResamplingParameters parameters;
			parameters.screenSpaceMotion = motionVector;
			parameters.sourceBufferIndex = RTXDISettings.InputBufferIndex;
			parameters.maxHistoryLength = 20;
			parameters.biasCorrectionMode = RTXDI_BIAS_CORRECTION_BASIC;
			parameters.depthThreshold = 0.1f;
			parameters.normalThreshold = 0.5f;
			parameters.numSamples = RTXDISettings.SpatioTemporalSamples;
			parameters.numDisocclusionBoostSamples = 0;
			parameters.samplingRadius = 32;
			parameters.enableVisibilityShortcut = true;
			parameters.enablePermutationSampling = true;
			parameters.discountNaiveSamples = false;
			parameters.uniformRandomNumber = RTXDISettings.UniformRandomNumber;

			int2 temporalSamplePixelPos = -1;
			reservoir = RTXDI_DISpatioTemporalResampling(pixelPosition, surface, reservoir, rng, RTXDISettings.RuntimeParameters, RTXDISettings.ReservoirBufferParameters, parameters, temporalSamplePixelPos, lightSample);

			if (RTXDI_IsValidDIReservoir(reservoir)) {
				if (RAB_GetConservativeVisibility(surface, lightSample)) {
					ShadeSurface(lightSample, surface, directDiffuse, directSpecular);
					const float invPDF = RTXDI_GetDIReservoirInvPdf(reservoir);
					directDiffuse *= invPDF;
					directSpecular *= invPDF;
				}
				else RTXDI_StoreVisibilityInDIReservoir(reservoir, 0, true);
			}
		}

		const NRDSettings NRDSettings = g_graphicsSettings.NRD;
		if (NRDSettings.Denoiser != NRDDenoiser::None) {
			const float3 Fenvironment = STL::BRDF::EnvironmentTerm_Rtg(Rf0, NoV, material.Roughness);
			const float3 diffuse = ((isDiffuse ? radiance : 0) + directDiffuse) / lerp((1 - Fenvironment) * albedo, 1, 0.01f), specular = ((isDiffuse ? 0 : radiance) + directSpecular) / lerp(Fenvironment, 1, 0.01f);
			if (NRDSettings.Denoiser == NRDDenoiser::ReBLUR) {
				const float normalizedHitDistance = REBLUR_FrontEnd_GetNormHitDist(hitDistance, linearDepth, NRDSettings.HitDistanceParameters, isDiffuse ? 1 : material.Roughness);
				noisyDiffuse = REBLUR_FrontEnd_PackRadianceAndNormHitDist(diffuse, normalizedHitDistance, true);
				noisySpecular = REBLUR_FrontEnd_PackRadianceAndNormHitDist(specular, normalizedHitDistance, true);
			}
			else if (NRDSettings.Denoiser == NRDDenoiser::ReLAX) {
				noisyDiffuse = RELAX_FrontEnd_PackRadianceAndHitDist(diffuse, hitDistance, true);
				noisySpecular = RELAX_FrontEnd_PackRadianceAndHitDist(specular, hitDistance, true);
			}
		}

		radiance += material.EmissiveColor + directDiffuse + directSpecular;
	}
	else if (!GetEnvironmentColor(rayDesc.Direction, radiance)) radiance = GetEnvironmentLightColor(rayDesc.Direction);

	g_color[pixelPosition] = radiance;
	g_noisyDiffuse[pixelPosition] = noisyDiffuse;
	g_noisySpecular[pixelPosition] = noisySpecular;

	if (RTXDISettings.IsEnabled) RTXDI_StoreDIReservoir(reservoir, RTXDISettings.ReservoirBufferParameters, pixelPosition, RTXDISettings.OutputBufferIndex);
}
