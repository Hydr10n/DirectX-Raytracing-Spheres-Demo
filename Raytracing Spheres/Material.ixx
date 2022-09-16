module;
#include <DirectXMath.h>

export module Material;

using namespace DirectX;

export struct Material {
	enum class Type { Lambertian, Metal, Dielectric, Isotropic, DiffuseLight } Type{};
	float RefractiveIndex{}, Roughness{};
	float _padding{};
	XMFLOAT4 Color{};

	static auto Lambertian(const XMFLOAT4& color) {
		return Material{
			.Type = Type::Lambertian,
			.Color = color
		};
	}

	static auto Metal(const XMFLOAT4& color, float roughness) {
		return Material{
			.Type = Type::Metal,
			.Roughness = roughness,
			.Color = color
		};
	}

	static auto Dielectric(const XMFLOAT4& color, float refractiveIndex) {
		return Material{
			.Type = Type::Dielectric,
			.RefractiveIndex = refractiveIndex,
			.Color = color
		};
	}

	static auto Isotropic(const XMFLOAT4& color) {
		return Material{
			.Type = Type::Isotropic,
			.Color = color
		};
	}

	static auto DiffuseLight(const XMFLOAT4& color) {
		return Material{
			.Type = Type::DiffuseLight,
			.Color = color
		};
	}
};
