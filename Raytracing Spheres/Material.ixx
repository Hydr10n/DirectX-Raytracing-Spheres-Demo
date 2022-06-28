module;
#include <DirectXMath.h>

export module Material;

export struct Material {
	enum class Type { Lambertian, Metal, Dielectric, Isotropic, DiffuseLight } Type{};
	float RefractiveIndex{}, Roughness{};
	float _padding{};
	DirectX::XMFLOAT4 Color{};

	auto& AsLambertian(const DirectX::XMFLOAT4& color) {
		Type = Type::Lambertian;
		Color = color;
		return *this;
	}

	auto& AsMetal(const DirectX::XMFLOAT4& color, float roughness) {
		Type = Type::Metal;
		Roughness = roughness;
		Color = color;
		return *this;
	}

	auto& AsDielectric(const DirectX::XMFLOAT4& color, float refractiveIndex) {
		Type = Type::Dielectric;
		RefractiveIndex = refractiveIndex;
		Color = color;
		return *this;
	}

	auto& AsIsotropic(const DirectX::XMFLOAT4& color) {
		Type = Type::Isotropic;
		Color = color;
		return *this;
	}

	auto& AsDiffuseLight(const DirectX::XMFLOAT4& color) {
		Type = Type::DiffuseLight;
		Color = color;
		return *this;
	}
};
