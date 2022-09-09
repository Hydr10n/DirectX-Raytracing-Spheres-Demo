module;
#include <DirectXMath.h>

#include <random>

export module Random;

using namespace DirectX;
using namespace std;

export struct Random {
	float Float(float min = 0, float max = 1) { return min + (max - min) * m_distribution(m_generator); }

	XMFLOAT2 Float2(float min = 0, float max = 1) { return { Float(min, max), Float(min, max) }; }

	XMFLOAT3 Float3(float min = 0, float max = 1) {
		const auto value = Float2(min, max);
		return { value.x, value.y, Float(min, max) };
	}

	XMFLOAT4 Float4(float min = 0, float max = 1) {
		const auto value = Float3(min, max);
		return { value.x, value.y, value.z, Float(min, max) };
	}

private:
	uniform_real_distribution<float> m_distribution = decltype(m_distribution)(0, 1);
	mt19937 m_generator = decltype(m_generator)(random_device()());
};
