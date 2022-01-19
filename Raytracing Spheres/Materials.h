#pragma once

#include <DirectXMath.h>

struct MaterialBase {
	enum class Type { Lambertian, Metal, Dielectric } Type;
	float RefractiveIndex;
	float Roughness;
	float Padding;
	DirectX::XMFLOAT4 Color;

	static MaterialBase CreateLambertian(const DirectX::XMFLOAT4& color) {
		return MaterialBase{
			.Type = Type::Lambertian,
			.Color = color
		};
	}

	static MaterialBase CreateMetal(const DirectX::XMFLOAT4& color, float roughness) {
		return MaterialBase{
			.Type = Type::Metal,
			.Roughness = roughness,
			.Color = color
		};
	}

	static MaterialBase CreateDielectric(const DirectX::XMFLOAT4& color, float refractiveIndex) {
		return MaterialBase{
			.Type = Type::Dielectric,
			.RefractiveIndex = refractiveIndex,
			.Color = color
		};
	}
};

struct Material : MaterialBase {
	size_t ConstantBufferIndex;

	Material(const MaterialBase& materialBase, size_t constantBufferIndex = SIZE_MAX) : MaterialBase(materialBase), ConstantBufferIndex(constantBufferIndex) {}
};
