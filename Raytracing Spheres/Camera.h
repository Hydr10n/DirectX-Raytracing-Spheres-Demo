#pragma once

#include "DirectXTK12/SimpleMath.h"

struct Camera {
	DirectX::XMFLOAT3 Position;
	float _padding;
	DirectX::XMMATRIX ProjectionToWorld;
};

class FirstPersonCamera {
public:
	void SetPosition(const DirectX::XMFLOAT3& position) {
		m_position = position;
		m_viewDirty = true;
	}

	const auto& GetPosition() const { return m_position; }

	void SetDirections(const DirectX::XMFLOAT3& rightDirection, const DirectX::XMFLOAT3& forwardDirection) {
		(m_rightDirection = rightDirection).Normalize();
		(m_forwardDirection = forwardDirection).Normalize();
		m_forwardDirection.Cross(m_rightDirection).Normalize(m_upDirection);
		m_viewDirty = true;
	}

	const auto& GetForwardDirection() const { return m_forwardDirection; }

	const auto& GetUpDirection() const { return m_upDirection; }

	const auto& GetRightDirection() const { return m_rightDirection; }

	void SetLens(float fovAngleY, float aspectRatio, float nearZ, float farZ) {
		m_projection = DirectX::XMMatrixPerspectiveFovLH(fovAngleY, aspectRatio, nearZ, farZ);
	}

	void UpdateView() {
		if (!m_viewDirty) return;
		m_rightDirection.Normalize();
		m_forwardDirection.Normalize();
		m_forwardDirection.Cross(m_rightDirection).Normalize(m_upDirection);
		m_view = DirectX::XMMatrixLookToLH(m_position, m_forwardDirection, m_upDirection);
		m_viewDirty = false;
	}

	const auto& GetView() const { return m_view; }

	const auto& GetProjection() const { return m_projection; }

	void Translate(const DirectX::XMFLOAT3& displacement) {
		m_position += displacement;
		m_viewDirty = true;
	}

	void Yaw(float angle) {
		using namespace DirectX::SimpleMath;
		const auto rotation = Matrix::CreateRotationY(angle);
		m_rightDirection = Vector3::Transform(m_rightDirection, rotation);
		m_forwardDirection = Vector3::Transform(m_forwardDirection, rotation);
		m_viewDirty = true;
	}

	void Pitch(float angle) {
		using namespace DirectX::SimpleMath;
		const auto rotation = Matrix::CreateFromAxisAngle(m_rightDirection, angle);
		m_upDirection = Vector3::Transform(m_upDirection, rotation);
		m_forwardDirection = Vector3::Transform(m_forwardDirection, rotation);
		m_viewDirty = true;
	}

private:
	DirectX::SimpleMath::Vector3 m_position, m_rightDirection{ 1, 0, 0 }, m_upDirection{ 0, 1, 0 }, m_forwardDirection{ 0, 0, 1 };

	bool m_viewDirty = true;
	DirectX::XMMATRIX m_view{}, m_projection{};
};
