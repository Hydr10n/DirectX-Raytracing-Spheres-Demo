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

	const RAB_Surface surface = RAB_GetGBufferSurface(pixelPosition, false);
	if (!RAB_IsSurfaceValid(surface))
	{
		return;
	}

	const ReSTIRDI_Parameters parameters = g_graphicsSettings.RTXDI.ReSTIRDI;

	RTXDI_DIReservoir reservoir = RTXDI_LoadDIReservoir(parameters.reservoirBufferParams, globalIndex, parameters.bufferIndices.spatialResamplingInputBufferIndex);

	RTXDI_DISpatialResamplingParameters spatialParameters;
	spatialParameters.sourceBufferIndex = parameters.bufferIndices.spatialResamplingInputBufferIndex;
	spatialParameters.numSamples = parameters.spatialResamplingParams.numSpatialSamples;
	spatialParameters.numDisocclusionBoostSamples = parameters.spatialResamplingParams.numDisocclusionBoostSamples;
	spatialParameters.targetHistoryLength = parameters.temporalResamplingParams.maxHistoryLength;
	spatialParameters.biasCorrectionMode = parameters.spatialResamplingParams.spatialBiasCorrection;
	spatialParameters.samplingRadius = parameters.spatialResamplingParams.spatialSamplingRadius;
	spatialParameters.depthThreshold = parameters.spatialResamplingParams.spatialDepthThreshold;
	spatialParameters.normalThreshold = parameters.spatialResamplingParams.spatialNormalThreshold;
	spatialParameters.enableMaterialSimilarityTest = true;
	spatialParameters.discountNaiveSamples = parameters.spatialResamplingParams.discountNaiveSamples;

	RAB_RandomSamplerState rng = RAB_InitRandomSampler(pixelPosition, 3);
	RAB_LightSample lightSample;
	reservoir = RTXDI_DISpatialResampling(pixelPosition, surface, reservoir, rng, g_graphicsSettings.RTXDI.Runtime, parameters.reservoirBufferParams, spatialParameters, lightSample);

	RTXDI_StoreDIReservoir(reservoir, parameters.reservoirBufferParams, globalIndex, parameters.bufferIndices.spatialResamplingOutputBufferIndex);
}
