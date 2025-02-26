#pragma once

namespace MeshHelpers
{
	uint3 Load3Indices(Buffer<uint> buffer, uint primitiveIndex)
	{
		const uint index = primitiveIndex * 3;
		return uint3(buffer[index], buffer[index + 1], buffer[index + 2]);
	}
}
