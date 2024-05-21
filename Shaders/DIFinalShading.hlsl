#include "RTXDIAppBridge.hlsli"
#include "rtxdi/DIReservoir.hlsli"

ROOT_SIGNATURE
[numthreads(RTXDI_SCREEN_SPACE_GROUP_SIZE, RTXDI_SCREEN_SPACE_GROUP_SIZE, 1)]
void main(uint2 globalIndex : SV_DispatchThreadID)
{
	if (any(globalIndex >= g_graphicsSettings.RenderSize))
	{
		return;
	}

	const uint2 pixelPosition = RTXDI_ReservoirPosToPixelPos(globalIndex, g_graphicsSettings.RTXDI.Runtime.activeCheckerboardField);

	const RAB_Surface surface = RAB_GetGBufferSurface(pixelPosition, false);
	if (RAB_IsSurfaceValid(surface))
	{
		const ReSTIRDI_Parameters DIParameters = g_graphicsSettings.RTXDI.ReSTIRDI;

		RTXDI_DIReservoir reservoir = RTXDI_LoadDIReservoir(DIParameters.reservoirBufferParams, globalIndex, DIParameters.bufferIndices.shadingInputBufferIndex);
		if (RTXDI_IsValidDIReservoir(reservoir))
		{
			const RAB_LightInfo lightInfo = RAB_LoadLightInfo(RTXDI_GetDIReservoirLightIndex(reservoir), false);
			const RAB_LightSample lightSample = RAB_SamplePolymorphicLight(lightInfo, surface, RTXDI_GetDIReservoirSampleUV(reservoir));

			if (DIParameters.initialSamplingParams.enableInitialVisibility
				&& !RAB_GetConservativeVisibility(surface, lightSample))
			{
				RTXDI_StoreVisibilityInDIReservoir(reservoir, 0, true);
				RTXDI_StoreDIReservoir(reservoir, DIParameters.reservoirBufferParams, globalIndex, DIParameters.bufferIndices.shadingInputBufferIndex);
				return;
			}

			float3 directDiffuse, directSpecular;
			ShadeSurface(lightSample, surface, directDiffuse, directSpecular);
			const float invPDF = RTXDI_GetDIReservoirInvPdf(reservoir);
			directDiffuse *= invPDF;
			directSpecular *= invPDF;

			if (g_graphicsSettings.NRD.Denoiser != NRDDenoiser::None)
			{
				PackNoisySignals(
					g_graphicsSettings.NRD,
					abs(dot(surface.Normal, surface.ViewDirection)), surface.LinearDepth,
					surface.Albedo, surface.Rf0, surface.Roughness,
					directDiffuse, directSpecular, length(lightSample.Position - surface.Position),
					g_noisyDiffuse[globalIndex], g_noisySpecular[globalIndex], true,
					g_noisyDiffuse[globalIndex], g_noisySpecular[globalIndex]
				);
			}

			g_color[globalIndex] += directDiffuse + directSpecular;
		}
	}
}
