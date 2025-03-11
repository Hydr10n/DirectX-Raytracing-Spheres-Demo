#pragma once

#define NRD_HEADER_ONLY
#include "NRDEncoding.hlsli"
#include "NRD.hlsli"

enum class Denoiser
{
	None, DLSSRayReconstruction, NRDReBLUR, NRDReLAX
};
