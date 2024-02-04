module;

#include "VertexTypes.h"

export module Vertex;

using namespace DirectX;

export {
	struct VertexDesc { UINT Stride{}, NormalOffset = ~0u, TextureCoordinateOffset = ~0u, TangentOffset = ~0u; };
}
