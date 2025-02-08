#include "RTXDIAppBridge.hlsli"
#include "Rtxdi/DI/InitialSampling.hlsli"

ROOT_SIGNATURE
[numthreads(RTXDI_SCREEN_SPACE_GROUP_SIZE, RTXDI_SCREEN_SPACE_GROUP_SIZE, 1)]
void main(uint2 globalIndex : SV_DispatchThreadID)
{
	const uint2 pixelPosition = RTXDI_ReservoirPosToPixelPos(globalIndex, g_graphicsSettings.RTXDI.Runtime.activeCheckerboardField);
	if (any(pixelPosition >= g_graphicsSettings.RenderSize))
	{
		return;
	}

	RTXDI_DIReservoir reservoir = RTXDI_EmptyDIReservoir();

	const ReSTIRDI_Parameters parameters = g_graphicsSettings.RTXDI.ReSTIRDI;

	const RAB_Surface surface = RAB_GetGBufferSurface(pixelPosition, false);
	if (RAB_IsSurfaceValid(surface))
	{
		RAB_RandomSamplerState
			rng = RAB_InitRandomSampler(pixelPosition, 1),
			tileRng = RAB_InitRandomSampler(pixelPosition / RTXDI_TILE_SIZE_IN_PIXELS, 1);

		const RTXDI_SampleParameters sampleParameters = RTXDI_InitSampleParameters(
			parameters.initialSamplingParams.numPrimaryLocalLightSamples,
			parameters.initialSamplingParams.numPrimaryInfiniteLightSamples,
			parameters.initialSamplingParams.numPrimaryEnvironmentSamples,
			parameters.initialSamplingParams.numPrimaryBrdfSamples,
			parameters.initialSamplingParams.brdfCutoff
		);

		RAB_LightSample lightSample;
		reservoir = RTXDI_SampleLightsForSurface(
			rng, tileRng,
			surface,
			sampleParameters,
			g_graphicsSettings.RTXDI.LightBuffer,
			parameters.initialSamplingParams.localLightSamplingMode,
#if RTXDI_ENABLE_PRESAMPLING
			g_graphicsSettings.RTXDI.LocalLightRISBufferSegment,
			g_graphicsSettings.RTXDI.EnvironmentLightRISBufferSegment,
#if RTXDI_REGIR_MODE != RTXDI_REGIR_DISABLED
			g_graphicsSettings.RTXDI.ReGIR,
#endif
#endif
			lightSample
		);
		if (parameters.initialSamplingParams.enableInitialVisibility
			&& RTXDI_IsValidDIReservoir(reservoir)
			&& !RAB_GetConservativeVisibility(surface, lightSample))
		{
			RTXDI_StoreVisibilityInDIReservoir(reservoir, 0, true);
		}
	}

	RTXDI_StoreDIReservoir(reservoir, parameters.reservoirBufferParams, globalIndex, parameters.bufferIndices.initialSamplingOutputBufferIndex);
}
