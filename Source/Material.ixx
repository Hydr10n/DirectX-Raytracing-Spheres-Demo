module;

#include <DirectXMath.h>

export module Material;

using namespace DirectX;

export {
	enum class AlphaMode { Opaque, Mask, Blend };

	struct Material {
		XMFLOAT4 BaseColor{ 0, 0, 0, 1 };
		float EmissiveStrength = 1;
		XMFLOAT3 EmissiveColor{};
		float Metallic{}, Roughness = 0.5f, IOR = 1.5f, Transmission{};
		AlphaMode AlphaMode = AlphaMode::Opaque;
		float AlphaCutoff = 0.5f;
		XMUINT2 _;
	};

	struct TextureMapType {
		enum : uint32_t {
			BaseColor,
			EmissiveColor,
			Metallic,
			Roughness,
			MetallicRoughness,
			Transmission,
			Normal,
			Count
		};
	};

	struct TextureMapInfo {
		uint32_t Descriptor = ~0u, TextureCoordinateIndex{};
		XMUINT2 _;
	};
}
