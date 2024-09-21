module;

#include <stdexcept>

#include <DirectXMath.h>

export module HaltonSamplePattern;

import ErrorHelpers;

using namespace DirectX;
using namespace ErrorHelpers;
using namespace std;

export class HaltonSamplePattern {
public:
	explicit HaltonSamplePattern(uint32_t sampleCount = ~0u) noexcept(false) : m_sampleCount(sampleCount) {
		if (!sampleCount) Throw<out_of_range>("Sample count cannot be 0");
	}

	static constexpr float Get(uint32_t index, uint32_t base) {
		float factor = 1, value = 0;
		for (; index > 0; index /= base) {
			factor /= static_cast<float>(base);
			value += factor * static_cast<float>(index % base);
		}
		return value - 0.5f;
	}

	static constexpr XMFLOAT2 Get(uint32_t index) { return { Get(index, 2), Get(index, 3) }; }

	XMFLOAT2 GetNext() noexcept {
		m_sampleIndex = m_sampleIndex % m_sampleCount + 1;
		return Get(m_sampleIndex);
	}

	uint32_t GetCount() const noexcept { return m_sampleCount; }
	uint32_t GetIndex() const noexcept { return m_sampleIndex; }
	void Reset() noexcept { m_sampleIndex = 0; }

private:
	uint32_t m_sampleCount, m_sampleIndex{};
};
