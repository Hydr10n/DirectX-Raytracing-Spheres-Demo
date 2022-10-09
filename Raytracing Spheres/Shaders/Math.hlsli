#pragma once

namespace Math {
	inline float SchlickFresnel(float cosine, float refractiveIndex) {
		float r0 = (1 - refractiveIndex) / (1 + refractiveIndex);
		r0 *= r0;
		return lerp(pow(1 - cosine, 5), 1, r0);
	}

	inline float3 SchlickFresnel(float cosine, float3 r0) { return lerp(pow(1 - cosine, 5), 1, r0); }

	inline float3x3 CalculateTBN(float3 N, float3 rayDirection) {
		const float3 T = normalize(cross(rayDirection, N)), B = cross(N, T);
		return float3x3(T, B, N);
	}
}
