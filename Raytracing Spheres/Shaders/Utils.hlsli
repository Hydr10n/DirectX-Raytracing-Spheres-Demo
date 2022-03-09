#ifndef UTILS_HLSLI
#define UTILS_HLSLI

inline RayDesc CreateRayDesc(float3 origin, float3 direction, float tMin = 1e-4, float tMax = 1e32) {
	const RayDesc ray = { origin, tMin, direction, tMax };
	return ray;
}

inline RayDesc GenerateCameraRay(uint2 raysIndex, uint2 raysDimensions, float3 origin, float4x4 projectionToWorld, float offset = 0.5f) {
	const float2 screenPosition = (raysIndex + offset) / raysDimensions * float2(2, -2) + float2(-1, 1);
	const float4 world = mul(float4(screenPosition, 0, 1), projectionToWorld);
	return CreateRayDesc(origin, normalize(world.xyz / world.w - origin));
}

inline float2 VertexAttribute(float2 attributes[3], float2 barycentrics) {
	return attributes[0] + barycentrics.x * (attributes[1] - attributes[0]) + barycentrics.y * (attributes[2] - attributes[0]);
}

inline float3 VertexAttribute(float3 attributes[3], float2 barycentrics) {
	return attributes[0] + barycentrics.x * (attributes[1] - attributes[0]) + barycentrics.y * (attributes[2] - attributes[0]);
}

inline uint3 Load3Indices(StructuredBuffer<uint> buffer, uint primitiveIndex = PrimitiveIndex()) {
	const uint index = primitiveIndex * 3;
	return uint3(buffer[index], buffer[index + 1], buffer[index + 2]);
}

inline float3x3 CalculateTBN(float3 normal, float3 rayDirection) {
	const float3 T = normalize(cross(rayDirection, normal)), B = cross(normal, T);
	return float3x3(T, B, normal);
}

#endif
