#pragma once

namespace MeshHelpers
{
	uint3 Load3x16BitIndices(ByteAddressBuffer buffer, uint primitiveIndex)
	{
		// ByteAddressBuffer::Load must be aligned at a 4-byte boundary.
		// Since we need to read 3 16-bit indices: { 0, 1, 2 }
		// aligned at a 4-byte boundary as: { 0 1 } { 2 0 } { 1 2 } { 0 1 } ...
		// we will load 8 bytes (~ 4 indices { a b | c d }) to handle two possible index triplet layouts,
		// based on the first index's offset in bytes being aligned at the 4-byte boundary or not:
		// Aligned:     { 0 1 | 2 - }
		// Not aligned: { - 0 | 1 2 }

		const uint offset = primitiveIndex * 3 * 2;
		const uint dwordAlignedOffset = offset & ~3;
		const uint2 four16BitIndices = buffer.Load2(dwordAlignedOffset);

		if (dwordAlignedOffset == offset)
		{
			// Aligned => retrieve the first 3 16-bit indices
			return float3(
				four16BitIndices.x & 0xffff,
				(four16BitIndices.x >> 16) & 0xffff,
				four16BitIndices.y & 0xffff
			);
		}
		
		// Not aligned => retrieve the last 3 16-bit indices
		return float3(
			(four16BitIndices.x >> 16) & 0xffff,
			four16BitIndices.y & 0xffff,
			(four16BitIndices.y >> 16) & 0xffff
		);
	}

	uint3 Load3Indices(Buffer<uint> buffer, uint primitiveIndex)
	{
		const uint index = primitiveIndex * 3;
		return uint3(buffer[index], buffer[index + 1], buffer[index + 2]);
	}
}
