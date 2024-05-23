#include "RTXDIAppBridge.hlsli"
#include "rtxdi/DIResamplingFunctions.hlsli"

ROOT_SIGNATURE
[numthreads(RTXDI_SCREEN_SPACE_GROUP_SIZE, RTXDI_SCREEN_SPACE_GROUP_SIZE, 1)]
void main(uint2 globalIndex : SV_DispatchThreadID)
{
	const uint2 pixelPosition = RTXDI_ReservoirPosToPixelPos(globalIndex, g_graphicsSettings.RTXDI.Runtime.activeCheckerboardField);
	if (any(pixelPosition >= g_graphicsSettings.RenderSize))
	{
		return;
	}

	const ReSTIRDI_Parameters DIParameters = g_graphicsSettings.RTXDI.ReSTIRDI;

	RTXDI_DIReservoir reservoir = RTXDI_EmptyDIReservoir();

	const RAB_Surface surface = RAB_GetGBufferSurface(pixelPosition, false);
	if (RAB_IsSurfaceValid(surface))
	{
		RAB_RandomSamplerState rng = RAB_InitRandomSampler(pixelPosition, 3);

		reservoir = RTXDI_LoadDIReservoir(DIParameters.reservoirBufferParams, globalIndex, DIParameters.bufferIndices.spatialResamplingInputBufferIndex);

		RTXDI_DISpatialResamplingParameters parameters;
		parameters.sourceBufferIndex = DIParameters.bufferIndices.spatialResamplingInputBufferIndex;
		parameters.numSamples = DIParameters.spatialResamplingParams.numSpatialSamples;
		parameters.numDisocclusionBoostSamples = DIParameters.spatialResamplingParams.numDisocclusionBoostSamples;
		parameters.targetHistoryLength = DIParameters.temporalResamplingParams.maxHistoryLength;
		parameters.biasCorrectionMode = DIParameters.spatialResamplingParams.spatialBiasCorrection;
		parameters.samplingRadius = DIParameters.spatialResamplingParams.spatialSamplingRadius;
		parameters.depthThreshold = DIParameters.spatialResamplingParams.spatialDepthThreshold;
		parameters.normalThreshold = DIParameters.spatialResamplingParams.spatialNormalThreshold;
		parameters.enableMaterialSimilarityTest = true;
		parameters.discountNaiveSamples = DIParameters.spatialResamplingParams.discountNaiveSamples;

		RAB_LightSample lightSample;
		reservoir = RTXDI_DISpatialResampling(pixelPosition, surface, reservoir, rng, g_graphicsSettings.RTXDI.Runtime, DIParameters.reservoirBufferParams, parameters, lightSample);
	}

	RTXDI_StoreDIReservoir(reservoir, DIParameters.reservoirBufferParams, globalIndex, DIParameters.bufferIndices.spatialResamplingOutputBufferIndex);
}
