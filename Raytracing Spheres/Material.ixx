module;

#include <DirectXMath.h>

export module Material;

using namespace DirectX;

export struct Material {
	XMFLOAT4 BaseColor{}, EmissiveColor{};
	XMFLOAT3 Specular{ 0.5f, 0.5f, 0.5f };
	float Metallic{}, Roughness = 0.5f, Opacity = 1, RefractiveIndex{};
	float _;
};
