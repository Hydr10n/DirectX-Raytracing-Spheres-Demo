#pragma once

#include <DirectXMath.h>

struct MaterialBase {
	enum class Type { Lambertian, Metal, Dielectric, Isotropic, DiffuseLight } Type{};
	float RefractiveIndex{};
	float Roughness{};
	float Padding{};
	DirectX::XMFLOAT4 Color{};

	static auto CreateLambertian(const DirectX::XMFLOAT4& color) {
		return MaterialBase{
			.Type = Type::Lambertian,
			.Color = color
		};
	}

	static auto CreateMetal(const DirectX::XMFLOAT4& color, float roughness) {
		return MaterialBase{
			.Type = Type::Metal,
			.Roughness = roughness,
			.Color = color
		};
	}

	static auto CreateDielectric(const DirectX::XMFLOAT4& color, float refractiveIndex) {
		return MaterialBase{
			.Type = Type::Dielectric,
			.RefractiveIndex = refractiveIndex,
			.Color = color
		};
	}

	static auto CreateIsotropic(const DirectX::XMFLOAT4& color) {
		return MaterialBase{
			.Type = Type::Isotropic,
			.Color = color
		};
	}

	static auto CreateDiffuseLight(const DirectX::XMFLOAT4& color) {
		return MaterialBase{
			.Type = Type::DiffuseLight,
			.Color = color
		};
	}
};

struct Material : MaterialBase {
	size_t ConstantBufferIndex = SIZE_MAX;

	Material() = default;

	Material(const MaterialBase& materialBase, size_t constantBufferIndex) : MaterialBase(materialBase), ConstantBufferIndex(constantBufferIndex) {}
};
