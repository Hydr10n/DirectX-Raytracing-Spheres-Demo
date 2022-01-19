#ifndef RAY_HLSLI
#define RAY_HLSLI

inline RayDesc CreateRayDesc(float3 origin, float3 direction, float tMin = 1e-4, float tMax = 1e32) {
	const RayDesc ray = { origin, tMin, direction, tMax };
	return ray;
}

// Generate a ray in world space for a camera pixel corresponding to an index from the dispatched 2D grid.
inline RayDesc GenerateCameraRay(uint3 raysIndex, uint3 raysDimensions, float3 origin, float4x4 projectionToWorld, float2 offset = 0.5) {
	const float2 xy = raysIndex.xy + offset; // Center in the middle of the pixel.

	float2 screenPos = xy / raysDimensions.xy * 2 - 1;
	screenPos.y = -screenPos.y; // Invert Y for DirectX-style coordinates.

	// Unproject the pixel coordinate into a ray.
	float4 world = mul(float4(screenPos, 0, 1), projectionToWorld);
	world.xyz /= world.w;
	
	return CreateRayDesc(origin, normalize(world.xyz - origin));
}

#endif
