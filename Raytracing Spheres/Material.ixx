module;

#include <DirectXMath.h>

export module Material;

using namespace DirectX;

export struct Material {
	XMFLOAT4 BaseColor{}, EmissiveColor{};
	float Metallic{}, Roughness = 0.5f, Opacity = 1, RefractiveIndex{};
	XMFLOAT4 _;
};
