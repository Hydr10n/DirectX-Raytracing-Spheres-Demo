module;

#include <DirectXMath.h>

export module Material;

using namespace DirectX;

export struct Material {
	XMFLOAT4 BaseColor{}, EmissiveColor{};
	float Roughness = 1;
	XMFLOAT3 Specular{ 0.5f, 0.5f, 0.5f };
	float Metallic{}, RefractiveIndex{};
	XMFLOAT2 _padding;
};
