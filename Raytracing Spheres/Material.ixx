module;

#include <DirectXMath.h>

export module Material;

using namespace DirectX;

export struct Material {
	enum class Type { Lambertian, Metal, Dielectric, Isotropic, DiffuseLight } Type;
	float RefractiveIndex, Roughness;
	float _padding;
	XMFLOAT4 BaseColor;

	static auto Lambertian(const XMFLOAT4& baseColor) {
		return Material{
			.Type = Type::Lambertian,
			.BaseColor = baseColor
		};
	}

	static auto Metal(const XMFLOAT4& baseColor, float roughness) {
		return Material{
			.Type = Type::Metal,
			.Roughness = roughness,
			.BaseColor = baseColor
		};
	}

	static auto Dielectric(const XMFLOAT4& baseColor, float refractiveIndex) {
		return Material{
			.Type = Type::Dielectric,
			.RefractiveIndex = refractiveIndex,
			.BaseColor = baseColor
		};
	}

	static auto Isotropic(const XMFLOAT4& baseColor) {
		return Material{
			.Type = Type::Isotropic,
			.BaseColor = baseColor
		};
	}

	static auto DiffuseLight(const XMFLOAT4& baseColor) {
		return Material{
			.Type = Type::DiffuseLight,
			.BaseColor = baseColor
		};
	}
};
