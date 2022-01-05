#pragma once

#include <cstdlib>

#include <DirectXMath.h>

struct Random {
	static float Float(float min = 0, float max = 1) { return min + (max - min) * rand() / (RAND_MAX + 1.0f); }

	static DirectX::XMFLOAT4 Float4(float min = 0, float max = 1) { return { Float(min, max), Float(min, max), Float(min, max), Float(min, max) }; }
};
