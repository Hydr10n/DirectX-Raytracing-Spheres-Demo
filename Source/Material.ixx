module;

#include <DirectXMath.h>

export module Material;

using namespace DirectX;

export {
	enum class AlphaMode { Opaque, Mask, Blend };

	struct Material {
		XMFLOAT4 BaseColor{ 0, 0, 0, 1 };
		XMFLOAT3 EmissiveColor{};
		float EmissiveStrength = 1, Metallic{}, Roughness = 0.5f, Transmission{}, IOR = 1;
		AlphaMode AlphaMode = AlphaMode::Opaque;
		float AlphaCutoff = 0.5f;
		XMUINT2 _;
	};

	enum class TextureMapType {
		BaseColor,
		EmissiveColor,
		Metallic,
		Roughness,
		MetallicRoughness,
		Transmission,
		Normal,
		Count
	};
}
