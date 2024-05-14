#include "RTXDIAppBridge.hlsli"
#include "rtxdi/DIResamplingFunctions.hlsli"
#include "rtxdi/InitialSamplingFunctions.hlsli"

[RootSignature(
	"RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED),"
	"StaticSampler(s0),"
	"SRV(t0),"
	"CBV(b0),"
	"CBV(b1),"
	"SRV(t1),"
	"SRV(t2),"
	"SRV(t3),"
	"SRV(t4),"
	"DescriptorTable(SRV(t5)),"
	"DescriptorTable(SRV(t6)),"
	"DescriptorTable(SRV(t7)),"
	"DescriptorTable(SRV(t8)),"
	"DescriptorTable(SRV(t9)),"
	"DescriptorTable(SRV(t10)),"
	"DescriptorTable(SRV(t11)),"
	"DescriptorTable(SRV(t12)),"
	"DescriptorTable(SRV(t13)),"
	"DescriptorTable(SRV(t14)),"
	"UAV(u0),"
	"DescriptorTable(UAV(u1)),"
	"DescriptorTable(UAV(u2)),"
	"DescriptorTable(UAV(u3))"
)]
[numthreads(16, 16, 1)]
void main(uint2 pixelPosition : SV_DispatchThreadID) {
	if (any(pixelPosition >= g_graphicsSettings.RenderSize)) return;
	pixelPosition = RTXDI_ReservoirPosToPixelPos(pixelPosition, g_graphicsSettings.RTXDI.RuntimeParameters.activeCheckerboardField);

	RTXDI_DIReservoir reservoir = RTXDI_EmptyDIReservoir();

	const RTXDISettings RTXDISettings = g_graphicsSettings.RTXDI;

	const RAB_Surface surface = RAB_GetGBufferSurface(pixelPosition, false);
	if (RAB_IsSurfaceValid(surface)) {
		STL::Rng::Hash::Initialize(pixelPosition, g_graphicsSettings.FrameIndex);

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

		if (RTXDI_IsValidDIReservoir(reservoir) && !BRDFSelected && !RAB_GetConservativeVisibility(surface, lightSample)) {
			RTXDI_StoreVisibilityInDIReservoir(reservoir, 0, true);
		}

		RTXDI_DISpatioTemporalResamplingParameters parameters;
		parameters.screenSpaceMotion = g_motionVectors[pixelPosition];
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
				const float directHitDistance = length(lightSample.Position - surface.Position);
				float3 directDiffuse, directSpecular;
				ShadeSurface(lightSample, surface, directDiffuse, directSpecular);
				const float invPDF = RTXDI_GetDIReservoirInvPdf(reservoir);
				directDiffuse *= invPDF;
				directSpecular *= invPDF;

				float4 noisyDiffuse = g_noisyDiffuse[pixelPosition], noisySpecular = g_noisySpecular[pixelPosition];
				const NRDSettings NRDSettings = g_graphicsSettings.NRD;
				if (NRDSettings.Denoiser != NRDDenoiser::None) {
					float indirectDiffuseHitDistance = 1.#INFf, indirectSpecularHitDistance = 1.#INFf;
					if (NRDSettings.Denoiser == NRDDenoiser::ReBLUR) {
						noisyDiffuse = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(noisyDiffuse);
						noisySpecular = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(noisySpecular);
						indirectDiffuseHitDistance = REBLUR_GetHitDist(noisyDiffuse.a, surface.LinearDepth, g_graphicsSettings.NRD.HitDistanceParameters, surface.Roughness);
						indirectSpecularHitDistance = REBLUR_GetHitDist(noisySpecular.a, surface.LinearDepth, g_graphicsSettings.NRD.HitDistanceParameters, surface.Roughness);
					}
					else if (NRDSettings.Denoiser == NRDDenoiser::ReLAX) {
						noisyDiffuse = RELAX_BackEnd_UnpackRadiance(noisyDiffuse);
						noisySpecular = RELAX_BackEnd_UnpackRadiance(noisySpecular);
						indirectDiffuseHitDistance = noisyDiffuse.a;
						indirectSpecularHitDistance = noisySpecular.a;
					}
					const bool isDiffuse = any(noisyDiffuse.rgb != 0);
					const float3
						Fenvironment = STL::BRDF::EnvironmentTerm_Rtg(surface.Rf0, abs(dot(surface.Normal, surface.ViewDirection)), surface.Roughness),
						diffuse = noisyDiffuse.rgb + directDiffuse / lerp((1 - Fenvironment) * surface.Albedo, 1, 0.01f),
						specular = noisySpecular.rgb + directSpecular / lerp(Fenvironment, 1, 0.01f);
					if (isDiffuse) {
						const float
							directLuminance = STL::Color::Luminance(directDiffuse),
							indirectLuminance = STL::Color::Luminance(noisyDiffuse.rgb),
							directHitDistanceContribution = min(directLuminance / (directLuminance + indirectLuminance + 1e-3f), 0.5f);
						indirectDiffuseHitDistance = lerp(indirectDiffuseHitDistance, directHitDistance, directHitDistanceContribution);
					}
					if (NRDSettings.Denoiser == NRDDenoiser::ReBLUR) {
						indirectDiffuseHitDistance = REBLUR_FrontEnd_GetNormHitDist(indirectDiffuseHitDistance, surface.LinearDepth, NRDSettings.HitDistanceParameters, isDiffuse ? 1 : surface.Roughness);
						noisyDiffuse = REBLUR_FrontEnd_PackRadianceAndNormHitDist(diffuse, indirectDiffuseHitDistance, true);
						noisySpecular = REBLUR_FrontEnd_PackRadianceAndNormHitDist(specular, indirectSpecularHitDistance, true);
					}
					else if (NRDSettings.Denoiser == NRDDenoiser::ReLAX) {
						noisyDiffuse = RELAX_FrontEnd_PackRadianceAndHitDist(diffuse, indirectDiffuseHitDistance, true);
						noisySpecular = RELAX_FrontEnd_PackRadianceAndHitDist(specular, indirectSpecularHitDistance, true);
					}
				}

				g_color[pixelPosition] += directDiffuse + directSpecular;
				g_noisyDiffuse[pixelPosition] = noisyDiffuse;
				g_noisySpecular[pixelPosition] = noisySpecular;
			}
			else RTXDI_StoreVisibilityInDIReservoir(reservoir, 0, true);
		}
	}

	RTXDI_StoreDIReservoir(reservoir, RTXDISettings.ReservoirBufferParameters, pixelPosition, RTXDISettings.OutputBufferIndex);
}
