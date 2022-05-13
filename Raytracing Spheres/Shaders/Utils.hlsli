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

inline float2 GetVertexAttribute(float2 attributes[3], float2 barycentrics) {
	return attributes[0] + barycentrics.x * (attributes[1] - attributes[0]) + barycentrics.y * (attributes[2] - attributes[0]);
}

inline float3 GetVertexAttribute(float3 attributes[3], float2 barycentrics) {
	return attributes[0] + barycentrics.x * (attributes[1] - attributes[0]) + barycentrics.y * (attributes[2] - attributes[0]);
}

inline uint GetTriangleBaseIndex(uint indexStrideInBytes, uint primitiveIndex = PrimitiveIndex()) {
	return indexStrideInBytes * primitiveIndex * 3;
}

// Load 3 16-bit indices from a ByteAddressBuffer.
inline uint3 Load3x16BitIndices(ByteAddressBuffer byteAddressBuffer, uint offset) {
	// ByteAddressBuffer::Load must be aligned at a 4-byte boundary.
	// Since we need to read 3 16-bit indices: { 0, 1, 2 }
	// aligned at a 4-byte boundary as: { 0 1 } { 2 0 } { 1 2 } { 0 1 } ...
	// we will load 8 bytes (~ 4 indices { a b | c d }) to handle two possible index triplet layouts,
	// based on the first index's offset in bytes being aligned at the 4-byte boundary or not:
	// Aligned:     { 0 1 | 2 - }
	// Not aligned: { - 0 | 1 2 }

	const uint dwordAlignedOffset = offset & ~3;
	const uint2 four16BitIndices = byteAddressBuffer.Load2(dwordAlignedOffset);

	uint3 indices;
	if (dwordAlignedOffset == offset) { // Aligned => retrieve the first 3 16-bit indices
		indices.x = four16BitIndices.x & 0xffff;
		indices.y = (four16BitIndices.x >> 16) & 0xffff;
		indices.z = four16BitIndices.y & 0xffff;
	}
	else { // Not aligned => retrieve the last 3 16-bit indices
		indices.x = (four16BitIndices.x >> 16) & 0xffff;
		indices.y = four16BitIndices.y & 0xffff;
		indices.z = (four16BitIndices.y >> 16) & 0xffff;
	}

	return indices;
}

inline float3x3 CalculateTBN(float3 normal, float3 rayDirection) {
	const float3 T = normalize(cross(rayDirection, normal)), B = cross(normal, T);
	return float3x3(T, B, normal);
}

#endif
