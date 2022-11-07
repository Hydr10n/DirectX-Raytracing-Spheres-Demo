module;

#include <DirectXMath.h>

#include <cmath>

#include <stdexcept>

export module HaltonSamplePattern;

using namespace DirectX;
using namespace std;

export struct HaltonSamplePattern {
	HaltonSamplePattern(uint32_t sampleCount = 16) : m_sampleCount(sampleCount) { if (!sampleCount) throw invalid_argument("Sample count cannot be 0"); }

	auto GetSampleCount() const { return m_sampleCount; }

	auto GetNext() const {
		constexpr auto Halton = [](uint32_t index, uint32_t base) {
			float factor = 1, ret = 0;
			for (; index > 0; index /= base) {
				factor /= static_cast<float>(base);
				ret += factor * (index % base);
			}
			return ret;
		};

		constexpr auto Fract = [](auto value) { return modf(value, &value); };

		const XMFLOAT2 value{ Halton(m_sampleIndex, 2), Halton(m_sampleIndex, 3) };

		m_sampleIndex = (m_sampleIndex + 1) % m_sampleCount;

		return XMFLOAT2(Fract(value.x + 0.5f) - 0.5f, Fract(value.y + 0.5f) - 0.5f);
	}

	void Reset() { m_sampleIndex = 0; }

private:
	uint32_t m_sampleCount;
	mutable uint32_t m_sampleIndex{};
};
