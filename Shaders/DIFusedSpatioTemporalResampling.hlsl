#include "RTXDIAppBridge.hlsli"
#include "rtxdi/DIResamplingFunctions.hlsli"

ROOT_SIGNATURE
[numthreads(RTXDI_SCREEN_SPACE_GROUP_SIZE, RTXDI_SCREEN_SPACE_GROUP_SIZE, 1)]
void main(uint2 globalIndex : SV_DispatchThreadID, uint2 localIndex : SV_GroupThreadID) {
	if (any(globalIndex >= g_graphicsSettings.RenderSize)) return;

	const ReSTIRDI_Parameters DIParameters = g_graphicsSettings.RTXDI.ReSTIRDI;

	const uint2 pixelPosition = RTXDI_ReservoirPosToPixelPos(globalIndex, g_graphicsSettings.RTXDI.Runtime.activeCheckerboardField);

	RTXDI_DIReservoir reservoir = RTXDI_EmptyDIReservoir();

	const RAB_Surface surface = RAB_GetGBufferSurface(pixelPosition, false);
	if (RAB_IsSurfaceValid(surface)) {
		RAB_RandomSamplerState rng = RAB_InitRandomSampler(pixelPosition, 2);

		reservoir = RTXDI_LoadDIReservoir(DIParameters.reservoirBufferParams, globalIndex, DIParameters.bufferIndices.initialSamplingOutputBufferIndex);

		RTXDI_DISpatioTemporalResamplingParameters parameters;
		parameters.screenSpaceMotion = g_motionVectors[pixelPosition];
		parameters.sourceBufferIndex = DIParameters.bufferIndices.temporalResamplingInputBufferIndex;
		parameters.maxHistoryLength = DIParameters.temporalResamplingParams.maxHistoryLength;
		parameters.biasCorrectionMode = DIParameters.temporalResamplingParams.temporalBiasCorrection;
		parameters.depthThreshold = DIParameters.temporalResamplingParams.temporalDepthThreshold;
		parameters.normalThreshold = DIParameters.temporalResamplingParams.temporalNormalThreshold;
		parameters.numSamples = DIParameters.spatialResamplingParams.numSpatialSamples + 1;
		parameters.numDisocclusionBoostSamples = DIParameters.spatialResamplingParams.numDisocclusionBoostSamples;
		parameters.samplingRadius = DIParameters.spatialResamplingParams.spatialSamplingRadius;
		parameters.enableVisibilityShortcut = DIParameters.temporalResamplingParams.discardInvisibleSamples;
		parameters.enablePermutationSampling = DIParameters.temporalResamplingParams.enablePermutationSampling;
		parameters.enableMaterialSimilarityTest = true;
		parameters.uniformRandomNumber = DIParameters.temporalResamplingParams.uniformRandomNumber;
		parameters.discountNaiveSamples = DIParameters.spatialResamplingParams.discountNaiveSamples;

		RAB_LightSample lightSample;
		int2 temporalSamplePixelPos;
		reservoir = RTXDI_DISpatioTemporalResampling(pixelPosition, surface, reservoir, rng, g_graphicsSettings.RTXDI.Runtime, DIParameters.reservoirBufferParams, parameters, temporalSamplePixelPos, lightSample);
	}

	if (DIParameters.temporalResamplingParams.enableBoilingFilter) {
		RTXDI_BoilingFilter(localIndex, DIParameters.temporalResamplingParams.boilingFilterStrength, reservoir);
	}

	RTXDI_StoreDIReservoir(reservoir, DIParameters.reservoirBufferParams, globalIndex, DIParameters.bufferIndices.shadingInputBufferIndex);
}
