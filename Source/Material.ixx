module;

#include <DirectXMath.h>

#include <Windows.h>

export module Material;

using namespace DirectX;

export {
	enum class AlphaMode { Opaque, Blend, Mask };

	struct Material {
		XMFLOAT4 BaseColor{};
		XMFLOAT3 EmissiveColor{};
		float EmissiveIntensity = 1, Metallic{}, Roughness = 0.5f, Transmission{}, IOR = 1;
		AlphaMode AlphaMode = AlphaMode::Opaque;
		float AlphaThreshold = 0.5f;
		BOOL HasTexture{};
		UINT _;
	};

	enum class TextureMapType {
		BaseColor,
		EmissiveColor,
		Metallic,
		Roughness,
		AmbientOcclusion,
		Transmission,
		Opacity,
		Normal,
		Count
	};
}
