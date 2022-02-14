#ifndef UTILS_HLSLI
#define UTILS_HLSLI

inline float2 VertexAttribute(float2 vertexAttributes[3], float2 barycentrics) {
	return vertexAttributes[0]
		+ barycentrics.x * (vertexAttributes[1] - vertexAttributes[0])
		+ barycentrics.y * (vertexAttributes[2] - vertexAttributes[0]);
}

inline float3 VertexAttribute(float3 vertexAttributes[3], float2 barycentrics) {
	return vertexAttributes[0]
		+ barycentrics.x * (vertexAttributes[1] - vertexAttributes[0])
		+ barycentrics.y * (vertexAttributes[2] - vertexAttributes[0]);
}

inline uint GetTriangleBaseIndex(uint indexStrideInBytes, uint primitiveIndex = PrimitiveIndex()) {
	return indexStrideInBytes * primitiveIndex * 3;
}

// Load 3 16-bit indices from a byte addressed buffer.
inline uint3 Load3x16BitIndices(ByteAddressBuffer byteAddressBuffer, uint offset) {
	// ByteAddressBuffer loads must be aligned at a 4 byte boundary.
	// Since we need to read 3 16-bit indices: { 0, 1, 2 }
	// aligned at a 4 byte boundary as: { 0 1 } { 2 0 } { 1 2 } { 0 1 } ...
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

// http://www.opengl-tutorial.org/intermediate-tutorials/tutorial-13-normal-mapping/
inline float3 CalculateTangent(float3 positions[3], float2 textureCoordinates[3]) {
	const float2 d0 = textureCoordinates[1] - textureCoordinates[0], d1 = textureCoordinates[2] - textureCoordinates[0];
	return ((positions[1] - positions[0]) * d1.y - (positions[2] - positions[0]) * d0.y) / (d0.x * d1.y - d0.y * d1.x);
}

#endif
