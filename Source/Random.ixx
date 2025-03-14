module;

#include <random>

#include <DirectXMath.h>

export module Random;

using namespace DirectX;
using namespace std;

export struct Random {
	explicit Random(unsigned int seed = random_device()()) : m_generator(seed) {}

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
	mt19937 m_generator;
	uniform_real_distribution<float> m_distribution = decltype(m_distribution)(0, 1);
};
