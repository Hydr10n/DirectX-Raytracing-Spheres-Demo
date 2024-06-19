module;

#include "MyAppData.h"

export module StringConverters;

using namespace DirectX;
using namespace sl;
using namespace WindowHelpers;

export {
	constexpr auto ToString(WindowMode value) {
		switch (value) {
			case WindowMode::Windowed: return "Windowed";
			case WindowMode::Borderless: return "Borderless";
			case WindowMode::Fullscreen: return "Fullscreen";
			default: throw;
		}
	}

	constexpr auto ToString(ReSTIRDI_LocalLightSamplingMode value) {
		switch (value) {
			case ReSTIRDI_LocalLightSamplingMode::Uniform: return "Uniform";
			case ReSTIRDI_LocalLightSamplingMode::Power_RIS: return "Power RIS";
			case ReSTIRDI_LocalLightSamplingMode::ReGIR_RIS: return "ReGIR RIS";
			default: throw;
		}
	}

	constexpr auto ToString(RTXGITechnique value) {
		switch (value) {
			case RTXGITechnique::None: return "None";
			case RTXGITechnique::SHARC: return "Spatially Hashed Radiance Cache";
			default: throw;
		}
	}

	constexpr auto ToString(ReflexMode value) {
		switch (value) {
			case ReflexMode::eOff: return "Off";
			case ReflexMode::eLowLatency: return "On";
			case ReflexMode::eLowLatencyWithBoost: return "On + Boost";
			default: throw;
		}
	}

	constexpr auto ToString(NRDDenoiser value) {
		switch (value) {
			case NRDDenoiser::None: return "None";
			case NRDDenoiser::ReBLUR: return "ReBLUR";
			case NRDDenoiser::ReLAX: return "ReLAX";
			default: throw;
		}
	}

	constexpr auto ToString(Upscaler value) {
		switch (value) {
			case Upscaler::None: return "None";
			case Upscaler::DLSS: return "NVIDIA DLSS";
			case Upscaler::XeSS: return "Intel XeSS";
			default: throw;
		}
	}

	constexpr auto ToString(SuperResolutionMode value) {
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

	constexpr auto ToString(ToneMapPostProcess::Operator value) {
		switch (value) {
			case ToneMapPostProcess::None: return "None";
			case ToneMapPostProcess::Saturate: return "Saturate";
			case ToneMapPostProcess::Reinhard: return "Reinhard";
			case ToneMapPostProcess::ACESFilmic: return "ACES Filmic";
			default: throw;
		}
	}

	constexpr auto ToString(ToneMapPostProcess::ColorPrimaryRotation value) {
		switch (value) {
			case ToneMapPostProcess::HDTV_to_UHDTV: return "Rec.709 to Rec.2020";
			case ToneMapPostProcess::DCI_P3_D65_to_UHDTV: return "DCI-P3-D65 to Rec.2020";
			case ToneMapPostProcess::HDTV_to_DCI_P3_D65: return "Rec.709 to DCI-P3-D65";
			default: throw;
		}
	}
}
