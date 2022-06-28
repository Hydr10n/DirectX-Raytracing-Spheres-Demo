#pragma once

#include "directxtk12/SimpleMath.h"

struct Camera {
	DirectX::XMFLOAT3 Position;
	float _padding;
	DirectX::XMMATRIX ProjectionToWorld;
};

class FirstPersonCamera {
public:
	const auto& GetPosition() const { return m_position; }

	void SetPosition(const DirectX::XMFLOAT3& position) {
		m_position = position;

		m_isViewChanged = true;
	}

	const auto& GetDirections() const { return m_directions; }

	void SetDirections(const DirectX::XMFLOAT3& rightDirection, const DirectX::XMFLOAT3& forwardDirection) {
		(m_directions.Right = rightDirection).Normalize();
		(m_directions.Forward = forwardDirection).Normalize();
		m_directions.Forward.Cross(m_directions.Right).Normalize(m_directions.Up);

		m_isViewChanged = true;
	}

	const auto& GetView() const {
		if (m_isViewChanged) {
			m_directions.Right.Normalize();
			m_directions.Forward.Normalize();
			m_directions.Forward.Cross(m_directions.Right).Normalize(m_directions.Up);

			m_view = XMMatrixLookToLH(m_position, m_directions.Forward, m_directions.Up);

			m_isViewChanged = false;
		}
		return m_view;
	}

	void Translate(const DirectX::XMFLOAT3& displacement) {
		m_position += displacement;

		m_isViewChanged = true;
	}

	void Yaw(float angle) {
		using namespace DirectX::SimpleMath;

		const auto rotation = Matrix::CreateRotationY(angle);
		m_directions.Right = Vector3::Transform(m_directions.Right, rotation);
		m_directions.Forward = Vector3::Transform(m_directions.Forward, rotation);

		m_isViewChanged = true;
	}

	void Pitch(float angle) {
		using namespace DirectX::SimpleMath;

		const auto rotation = Matrix::CreateFromAxisAngle(m_directions.Right, angle);
		m_directions.Up = Vector3::Transform(m_directions.Up, rotation);
		m_directions.Forward = Vector3::Transform(m_directions.Forward, rotation);

		m_isViewChanged = true;
	}

	const auto& GetProjection() const { return m_projection; }

	void SetLens(float fovAngleY, float aspectRatio, float nearZ, float farZ) {
		m_projection = DirectX::XMMatrixPerspectiveFovLH(fovAngleY, aspectRatio, nearZ, farZ);
	}

private:
	mutable bool m_isViewChanged = true;

	DirectX::SimpleMath::Vector3 m_position;

	mutable struct { DirectX::SimpleMath::Vector3 Right{ 1, 0, 0 }, Up{ 0, 1, 0 }, Forward{ 0, 0, 1 }; } m_directions;

	mutable DirectX::XMMATRIX m_view{};

	DirectX::XMMATRIX m_projection{};
};
