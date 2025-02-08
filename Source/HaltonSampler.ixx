module;

#include <stdexcept>

#include <DirectXMath.h>

#include "ml.h"
#include "ml.hlsli"

export module HaltonSampler;

import ErrorHelpers;

using namespace DirectX;
using namespace ErrorHelpers;
using namespace Sequence;
using namespace std;

#define GET(N) \
	auto ret = Get##N##D(m_index + 1); \
	m_index = (m_index + 1) % m_count; \
	return ret;

export class HaltonSampler {
public:
	explicit HaltonSampler(uint32_t count = ~0u) noexcept(false) : m_count(count) {
		if (!count) {
			Throw<out_of_range>("Sample count cannot be 0");
		}
	}

	static float Get1D(uint32_t index) { return Halton1D(index); }
	static XMFLOAT2 Get2D(uint32_t index) { return reinterpret_cast<const XMFLOAT2&>(Halton2D(index)); }
	static XMFLOAT3 Get3D(uint32_t index) { return reinterpret_cast<const XMFLOAT3&>(Halton3D(index)); }

	float GetNext1D() noexcept { GET(1); }
	XMFLOAT2 GetNext2D() noexcept { GET(2); }
	XMFLOAT3 GetNext3D() noexcept { GET(3); }

	uint32_t GetCount() const noexcept { return m_count; }
	uint32_t GetIndex() const noexcept { return m_index; }
	void Reset() noexcept { m_index = 0; }

private:
	uint32_t m_count, m_index{};
};
