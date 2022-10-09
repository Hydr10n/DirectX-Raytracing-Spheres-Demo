#pragma once

struct Random {
	int Seed;

	void Initialize(int val0, int val1, int backOff = 16) {
		int s = 0;
		[unroll]
		for (int n = 0; n < backOff; n++) {
			s += 0x9e3779b9;
			val0 += ((val1 << 4) + 0xa341316c) ^ (val1 + s) ^ ((val1 >> 5) + 0xc8013ea4);
			val1 += ((val0 << 4) + 0xad90777d) ^ (val0 + s) ^ ((val0 >> 5) + 0x7e95761e);
		}
		Seed = val0;
	}

	float Float(float min = 0, float max = 1) {
		Seed = 1664525 * Seed + 1013904223;
		return min + (max - min) * float(Seed & 0x00FFFFFF) / float(0x01000000);
	}

	float2 Float2(float min = 0, float max = 1) { return float2(Float(min, max), Float(min, max)); }

	float3 Float3(float min = 0, float max = 1) { return float3(Float2(min, max), Float(min, max)); }

	float3 InUnitSphere() {
		for (;;) {
			const float3 v = Float3(-1, 1);
			if (length(v) < 1) return v;
		}
	}

	float3 UnitVector() { return normalize(InUnitSphere()); }
};
