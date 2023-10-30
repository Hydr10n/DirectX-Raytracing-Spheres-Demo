module;

#include <DirectXMath.h>

#include <cmath>

#include <stdexcept>

export module HaltonSamplePattern;

using namespace DirectX;
using namespace std;

export class HaltonSamplePattern {
public:
	HaltonSamplePattern(uint32_t sampleCount = ~0u) noexcept(false) : m_sampleCount(sampleCount) { if (!sampleCount) throw invalid_argument("Sample count cannot be 0"); }

	static auto Get(uint32_t index, uint32_t base) {
		float factor = 1, value = 0;
		for (; index > 0; index /= base) {
			factor /= static_cast<float>(base);
			value += factor * (index % base);
		}
		return modf(value + 0.5f, &value) - 0.5f;
	}

	static auto Get(uint32_t index) { return XMFLOAT2(Get(index, 2), Get(index, 3)); }

	auto GetNext() const noexcept {
		const auto ret = Get(m_sampleIndex);
		m_sampleIndex = (m_sampleIndex + 1) % m_sampleCount;
		return ret;
	}

	auto GetCount() const noexcept { return m_sampleCount; }
	auto GetIndex() const noexcept { return m_sampleIndex; }
	void Reset() noexcept { m_sampleIndex = 0; }

private:
	uint32_t m_sampleCount;
	mutable uint32_t m_sampleIndex{};
};