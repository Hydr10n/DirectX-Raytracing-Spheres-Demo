#ifndef UTILS_HLSLI
#define UTILS_HLSLI

inline RayDesc CreateRayDesc(float3 origin, float3 direction, float tMin = 1e-4, float tMax = 1e32) {
	const RayDesc ray = { origin, tMin, direction, tMax };
	return ray;
}

inline RayDesc GenerateCameraRay(uint2 raysIndex, uint2 raysDimensions, float3 origin, float4x4 projectionToWorld, float2 offset = float2(0.5f, 0.5f)) {
	const float2 screenPosition = (raysIndex + offset) / raysDimensions * float2(2, -2) + float2(-1, 1);
	const float4 world = mul(float4(screenPosition, 0, 1), projectionToWorld);
	return CreateRayDesc(origin, normalize(world.xyz / world.w - origin));
}

#endif
