#include "RtxdiAppBridge.hlsli"
#include "rtxdi/PresamplingFunctions.hlsli"

ROOT_SIGNATURE
[numthreads(RTXDI_PRESAMPLING_GROUP_SIZE, 1, 1)]
void main(uint2 globalIndex : SV_DispatchThreadID)
{
	uint2 textureSize;
	g_localLightPDF.GetDimensions(textureSize.x, textureSize.y);
	RAB_RandomSamplerState rng = RAB_InitRandomSampler(globalIndex.xy, 0);
	RTXDI_PresampleLocalLights(rng, g_localLightPDF, textureSize, globalIndex.y, globalIndex.x, g_graphicsSettings.RTXDI.LightBuffer.localLightBufferRegion, g_graphicsSettings.RTXDI.LocalLightRISBufferSegment);
}
