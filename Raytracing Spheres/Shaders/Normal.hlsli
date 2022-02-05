#ifndef NORMAL_HLSLI
#define NORMAL_HLSLI

inline float3x3 CalculateTBN(float3 normal, float3 rayDirection) {
	const float3 t = normalize(cross(rayDirection, normal)), b = cross(normal, t);
	return float3x3(t, b, normal);
}

inline float3 TwoChannelNormalX2(float2 normal) {
	const float2 xy = 2 * normal - 1;
	return float3(xy, sqrt(1 - dot(xy, xy)));
}

#endif
