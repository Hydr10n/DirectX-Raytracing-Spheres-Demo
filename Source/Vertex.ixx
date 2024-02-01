module;

#include "VertexTypes.h"

export module Vertex;

using namespace DirectX;

export {
	struct VertexDesc {
		UINT Stride;
		XMUINT3 _;
		struct { UINT Normal = ~0u, TextureCoordinate = ~0u, Tangent = ~0u, _; } Offsets;
	};
}
