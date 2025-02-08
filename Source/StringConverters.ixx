module;

#include "directxtk12/PostProcess.h"

#include "Rtxdi/DI/ReSTIRDIParameters.h"

#include "sl_helpers.h"

export module StringConverters;

import Denoiser;
import RTXGI;
import Upscaler;
import WindowHelpers;

using namespace DirectX;
using namespace sl;
using namespace std;
using namespace WindowHelpers;

export {
	constexpr string ToString(WindowMode value) {
		switch (value) {
			case WindowMode::Windowed: return "Windowed";
			case WindowMode::Borderless: return "Borderless";
			case WindowMode::Fullscreen: return "Fullscreen";
			default: throw;
		}
	}

	constexpr string ToString(ReSTIRDI_LocalLightSamplingMode value) {
		switch (value) {
			case ReSTIRDI_LocalLightSamplingMode::Uniform: return "Uniform";
			case ReSTIRDI_LocalLightSamplingMode::Power_RIS: return "Power RIS";
			case ReSTIRDI_LocalLightSamplingMode::ReGIR_RIS: return "ReGIR RIS";
			default: throw;
		}
	}

	constexpr string ToString(ReSTIRDI_TemporalBiasCorrectionMode value) {
		switch (value) {
			case ReSTIRDI_TemporalBiasCorrectionMode::Off: return "Off";
			case ReSTIRDI_TemporalBiasCorrectionMode::Basic: return "Basic";
			case ReSTIRDI_TemporalBiasCorrectionMode::Pairwise: return "Pairwise";
			case ReSTIRDI_TemporalBiasCorrectionMode::Raytraced: return "Raytraced";
			default: throw;
		}
	}

	constexpr string ToString(ReSTIRDI_SpatialBiasCorrectionMode value) {
		switch (value) {
			case ReSTIRDI_SpatialBiasCorrectionMode::Off: return "Off";
			case ReSTIRDI_SpatialBiasCorrectionMode::Basic: return "Basic";
			case ReSTIRDI_SpatialBiasCorrectionMode::Pairwise: return "Pairwise";
			case ReSTIRDI_SpatialBiasCorrectionMode::Raytraced: return "Raytraced";
			default: throw;
		}
	}

	constexpr string ToString(RTXGITechnique value) {
		switch (value) {
			case RTXGITechnique::None: return "None";
			case RTXGITechnique::SHARC: return "SHARC";
			default: throw;
		}
	}

	constexpr string ToString(ReflexMode value) {
		switch (value) {
			case ReflexMode::eOff: return "Off";
			case ReflexMode::eLowLatency: return "On";
			case ReflexMode::eLowLatencyWithBoost: return "On + Boost";
			default: throw;
		}
	}

	constexpr string ToString(Denoiser value) {
		switch (value) {
			case Denoiser::None: return "None";
			case Denoiser::DLSSRayReconstruction: return "NVIDIA DLSS Ray Reconstruction";
			case Denoiser::NRDReBLUR: return "NVIDIA ReBLUR";
			case Denoiser::NRDReLAX: return "NVIDIA ReLAX";
			default: throw;
		}
	}

	constexpr string ToString(Upscaler value) {
		switch (value) {
			case Upscaler::None: return "None";
			case Upscaler::DLSS: return "NVIDIA DLSS";
			case Upscaler::XeSS: return "Intel XeSS";
			default: throw;
		}
	}

	constexpr string ToString(SuperResolutionMode value) {
		switch (value) {
			case SuperResolutionMode::Auto: return "Auto";
			case SuperResolutionMode::Native: return "Native";
			case SuperResolutionMode::Quality: return "Quality";
			case SuperResolutionMode::Balanced: return "Balanced";
			case SuperResolutionMode::Performance: return "Performance";
			case SuperResolutionMode::UltraPerformance: return "Ultra Performance";
			default: throw;
		}
	}

	constexpr string ToString(ToneMapPostProcess::Operator value) {
		switch (value) {
			case ToneMapPostProcess::Saturate: return "Saturate";
			case ToneMapPostProcess::Reinhard: return "Reinhard";
			case ToneMapPostProcess::ACESFilmic: return "ACES Filmic";
			default: throw;
		}
	}

	constexpr string ToString(ToneMapPostProcess::ColorPrimaryRotation value) {
		switch (value) {
			case ToneMapPostProcess::HDTV_to_UHDTV: return "Rec.709 to Rec.2020";
			case ToneMapPostProcess::DCI_P3_D65_to_UHDTV: return "DCI-P3-D65 to Rec.2020";
			case ToneMapPostProcess::HDTV_to_DCI_P3_D65: return "Rec.709 to DCI-P3-D65";
			default: throw;
		}
	}
}
