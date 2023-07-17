module;

#include <DirectXMath.h>

#include <random>

export module Random;

using namespace DirectX;
using namespace std;

export struct Random {
	Random(unsigned int seed = random_device()()) : m_generator(seed) {}

	float Float(float min = 0, float max = 1) const { return min + (max - min) * m_distribution(m_generator); }

	XMFLOAT2 Float2(float min = 0, float max = 1) const { return { Float(min, max), Float(min, max) }; }

	XMFLOAT3 Float3(float min = 0, float max = 1) const {
		const auto value = Float2(min, max);
		return { value.x, value.y, Float(min, max) };
	}

	XMFLOAT4 Float4(float min = 0, float max = 1) const {
		const auto value = Float3(min, max);
		return { value.x, value.y, value.z, Float(min, max) };
	}

private:
	mutable mt19937 m_generator;
	mutable uniform_real_distribution<float> m_distribution = decltype(m_distribution)(0, 1);
};
