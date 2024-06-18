#include "RTXDIAppBridge.hlsli"
#include "rtxdi/DIResamplingFunctions.hlsli"

ROOT_SIGNATURE
[numthreads(RTXDI_SCREEN_SPACE_GROUP_SIZE, RTXDI_SCREEN_SPACE_GROUP_SIZE, 1)]
void main(uint2 globalIndex : SV_DispatchThreadID, uint2 localIndex : SV_GroupThreadID)
{
	const uint2 pixelPosition = RTXDI_ReservoirPosToPixelPos(globalIndex, g_graphicsSettings.RTXDI.Runtime.activeCheckerboardField);
	if (any(pixelPosition >= g_graphicsSettings.RenderSize))
	{
		return;
	}

	const ReSTIRDI_Parameters parameters = g_graphicsSettings.RTXDI.ReSTIRDI;

	RTXDI_DIReservoir reservoir = RTXDI_EmptyDIReservoir();

	const RAB_Surface surface = RAB_GetGBufferSurface(pixelPosition, false);
	if (RAB_IsSurfaceValid(surface))
	{
		RAB_RandomSamplerState rng = RAB_InitRandomSampler(pixelPosition, 2);

		RTXDI_DITemporalResamplingParameters temporalParameters;
		temporalParameters.screenSpaceMotion = g_motionVectors[pixelPosition];
		temporalParameters.sourceBufferIndex = parameters.bufferIndices.temporalResamplingInputBufferIndex;
		temporalParameters.maxHistoryLength = parameters.temporalResamplingParams.maxHistoryLength;
		temporalParameters.biasCorrectionMode = parameters.temporalResamplingParams.temporalBiasCorrection;
		temporalParameters.depthThreshold = parameters.temporalResamplingParams.temporalDepthThreshold;
		temporalParameters.normalThreshold = parameters.temporalResamplingParams.temporalNormalThreshold;
		temporalParameters.enableVisibilityShortcut = parameters.temporalResamplingParams.discardInvisibleSamples;
		temporalParameters.enablePermutationSampling = parameters.temporalResamplingParams.enablePermutationSampling;
		temporalParameters.uniformRandomNumber = parameters.temporalResamplingParams.uniformRandomNumber;

		RAB_LightSample lightSample;
		int2 temporalSamplePixelPos;
		reservoir = RTXDI_LoadDIReservoir(parameters.reservoirBufferParams, globalIndex, parameters.bufferIndices.initialSamplingOutputBufferIndex);
		reservoir = RTXDI_DITemporalResampling(pixelPosition, surface, reservoir, rng, g_graphicsSettings.RTXDI.Runtime, parameters.reservoirBufferParams, temporalParameters, temporalSamplePixelPos, lightSample);
	}

#ifdef RTXDI_ENABLE_BOILING_FILTER
	if (parameters.temporalResamplingParams.enableBoilingFilter)
	{
		RTXDI_BoilingFilter(localIndex, parameters.temporalResamplingParams.boilingFilterStrength, reservoir);
	}
#endif

	RTXDI_StoreDIReservoir(reservoir, parameters.reservoirBufferParams, globalIndex, parameters.bufferIndices.temporalResamplingOutputBufferIndex);
}
