module;

#include "MyAppData.h"

export module StringConverters;

using namespace DirectX;
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
			case Upscaler::FSR: return "AMD FSR";
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
}
