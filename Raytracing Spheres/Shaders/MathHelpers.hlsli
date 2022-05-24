#ifndef MATHHELPERS_HLSLI
#define MATHHELPERS_HLSLI

inline float3x3 CalculateTBN(float3 normal, float3 rayDirection) {
	const float3 T = normalize(cross(rayDirection, normal)), B = cross(normal, T);
	return float3x3(T, B, normal);
}

#endif
