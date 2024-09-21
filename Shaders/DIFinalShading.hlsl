#include "RTXDIAppBridge.hlsli"
#include "rtxdi/DIReservoir.hlsli"
#include "rtxdi/ReGIRSampling.hlsli"

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

	RTXDI_DIReservoir reservoir = RTXDI_LoadDIReservoir(parameters.reservoirBufferParams, globalIndex, parameters.bufferIndices.shadingInputBufferIndex);
	if (!RTXDI_IsValidDIReservoir(reservoir))
	{
		return;
	}

	const RAB_LightInfo lightInfo = RAB_LoadLightInfo(RTXDI_GetDIReservoirLightIndex(reservoir), false);
	const RAB_LightSample lightSample = RAB_SamplePolymorphicLight(lightInfo, surface, RTXDI_GetDIReservoirSampleUV(reservoir));

	float3 visibility;
	bool visibilityReused = false;
	if (parameters.shadingParams.reuseFinalVisibility)
	{
		RTXDI_VisibilityReuseParameters visibilityReuseParameters;
		visibilityReuseParameters.maxAge = parameters.shadingParams.finalVisibilityMaxAge;
		visibilityReuseParameters.maxDistance = parameters.shadingParams.finalVisibilityMaxDistance;
		visibilityReused = RTXDI_GetDIReservoirVisibility(reservoir, visibilityReuseParameters, visibility);
	}
	if (!visibilityReused)
	{
		visibility = GetFinalVisibility(surface, lightSample.Position);
		RTXDI_StoreVisibilityInDIReservoir(reservoir, visibility, parameters.temporalResamplingParams.discardInvisibleSamples);
		RTXDI_StoreDIReservoir(reservoir, parameters.reservoirBufferParams, globalIndex, parameters.bufferIndices.shadingInputBufferIndex);
	}

	if (all(visibility == 0))
	{
		return;
	}

	float3 diffuse, specular;
	if (!surface.Shade(lightSample, diffuse, specular))
	{
		return;
	}

	const float invPDF = RTXDI_GetDIReservoirInvPdf(reservoir);
	diffuse *= invPDF * visibility;
	specular *= invPDF * visibility;

	if (g_graphicsSettings.IsReGIRCellVisualizationEnabled
		&& parameters.initialSamplingParams.localLightSamplingMode == ReSTIRDI_LocalLightSamplingMode_REGIR_RIS)
	{
		const float3 cellColor = RTXDI_VisualizeReGIRCells(g_graphicsSettings.RTXDI.ReGIR, surface.Position);
		diffuse = lerp(diffuse, cellColor, 0.5f);
	}

	const float3 radiance = diffuse + specular;
	if (!NRD_IsValidRadiance(radiance))
	{
		return;
	}

	g_color[pixelPosition] += radiance;

	if (g_graphicsSettings.NRD.Denoiser != NRDDenoiser::None)
	{
		PackNoisySignals(
			g_graphicsSettings.NRD,
			abs(dot(surface.Normal, surface.ViewDirection)), surface.LinearDepth,
			surface.BRDFSample,
			diffuse, specular, length(lightSample.Position - surface.Position),
			g_noisyDiffuse[pixelPosition], g_noisySpecular[pixelPosition], true,
			g_noisyDiffuse[pixelPosition], g_noisySpecular[pixelPosition]
		);
	}
}
