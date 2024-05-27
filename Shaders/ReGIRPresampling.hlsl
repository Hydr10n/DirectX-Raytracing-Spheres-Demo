#include "RtxdiAppBridge.hlsli"
#include "rtxdi/PresamplingFunctions.hlsli"

ROOT_SIGNATURE
[numthreads(RTXDI_PRESAMPLING_GROUP_SIZE, 1, 1)]
void main(uint globalIndex : SV_DispatchThreadID)
{
	RAB_RandomSamplerState
		rng = RAB_InitRandomSampler(uint2(globalIndex & 0xfff, globalIndex >> 12), 1),
		coherentRng = RAB_InitRandomSampler(uint2(globalIndex >> 8, 0), 1);
	RTXDI_PresampleLocalLightsForReGIR(rng, coherentRng, globalIndex, g_graphicsSettings.RTXDI.LightBuffer.localLightBufferRegion, g_graphicsSettings.RTXDI.LocalLightRISBufferSegment, g_graphicsSettings.RTXDI.ReGIR);
}
