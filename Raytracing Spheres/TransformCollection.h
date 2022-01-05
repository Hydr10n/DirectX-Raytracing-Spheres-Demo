#pragma once

#include <DirectXMath.h>

#include <vector>

struct Transform {
	enum class Type { RotationY, Scaling, Translation } Type;

	union {
		float RotationY;
		DirectX::XMFLOAT3 Scaling;
		DirectX::XMFLOAT3 Translation;
	};

	static Transform CreateRotationY(float rotationY = 0) {
		return Transform{
			.Type = Type::RotationY,
			.RotationY = rotationY
		};
	}

	static Transform CreateScaling(const DirectX::XMFLOAT3& scaling = { 1, 1, 1 }) {
		return Transform{
			.Type = Type::Scaling,
			.Scaling = scaling
		};
	}

	static Transform CreateTranslation(const DirectX::XMFLOAT3& translation = {}) {
		return Transform{
			.Type = Type::Translation,
			.Translation = translation
		};
	}
};

struct TransformCollection : std::vector<Transform> {
	TransformCollection() { push_back(Transform::CreateTranslation()); }

	TransformCollection(const std::initializer_list<Transform>& transforms) { assign(transforms.begin(), transforms.end()); }

	DirectX::XMMATRIX Transform() const {
		using namespace DirectX;
		XMMATRIX ret = XMMatrixIdentity();
		for (const auto& transform : *this) {
			switch (transform.Type) {
			case Transform::Type::RotationY: ret *= XMMatrixRotationY(transform.RotationY); break;
			case Transform::Type::Scaling: ret *= XMMatrixScalingFromVector(XMLoadFloat3(&transform.Scaling)); break;
			case Transform::Type::Translation: ret *= XMMatrixTranslationFromVector(XMLoadFloat3(&transform.Translation)); break;
			}
		}
		return ret;
	}
};
