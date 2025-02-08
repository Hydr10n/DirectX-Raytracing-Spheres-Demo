export module Upscaler;

export import Streamline;
export import XeSS;

export {
	enum class Upscaler { None, DLSS, XeSS };

	enum class SuperResolutionMode { Auto, Native, Quality, Balanced, Performance, UltraPerformance };
}
