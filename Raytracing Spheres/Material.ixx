module;

#include <DirectXMath.h>

export module Material;

using namespace DirectX;

export {
	enum class AlphaMode { Opaque, Blend, Mask };

	struct Material {
		XMFLOAT4 BaseColor{}, EmissiveColor{};
		float Metallic{}, Roughness = 0.5f, Opacity = 1, RefractiveIndex = 1;
		AlphaMode AlphaMode = AlphaMode::Opaque;
		float AlphaThreshold = 0.5f;
		float _;
	};
}
