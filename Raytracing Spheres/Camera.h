#pragma once

#include <SimpleMath.h>

class Camera {
public:
	Camera(bool isLeftHandedCoordinateSystem) :
		m_isLeftHandedCoordinateSystem(isLeftHandedCoordinateSystem),
		m_forwardDirection(isLeftHandedCoordinateSystem ? DirectX::XMFLOAT3(0, 0, 1) : DirectX::XMFLOAT3(0, 0, -1)) {}

	void SetPosition(const DirectX::XMFLOAT3& position) {
		m_position = position;
		m_viewDirty = true;
	}

	auto GetPosition() const { return m_position; }

	void SetDirections(const DirectX::XMFLOAT3& forwardDirection, const DirectX::XMFLOAT3& upDirection) {
		(m_forwardDirection = forwardDirection).Normalize();
		(m_upDirection = upDirection).Normalize();
		const decltype(m_upDirection) (&directions)[] { m_upDirection, forwardDirection };
		directions[!m_isLeftHandedCoordinateSystem].Cross(directions[m_isLeftHandedCoordinateSystem]).Normalize(m_rightDirection);
		m_viewDirty = true;
	}

	auto GetForwardDirection() const { return m_forwardDirection; }

	auto GetUpDirection() const { return m_upDirection; }

	auto GetRightDirection() const { return m_rightDirection; }

	void SetLens(float fovAngleY, float aspectRatio, float nearZ, float farZ) {
		m_fovAngleY = fovAngleY;
		m_aspectRatio = aspectRatio;
		m_nearZ = nearZ;
		m_farZ = farZ;
		m_projection = m_isLeftHandedCoordinateSystem ?
			DirectX::XMMatrixPerspectiveFovLH(fovAngleY, aspectRatio, nearZ, farZ) :
			DirectX::XMMatrixPerspectiveFovRH(fovAngleY, aspectRatio, nearZ, farZ);
	}

	void UpdateView() {
		if (!m_viewDirty) return;
		const decltype(m_forwardDirection) (&directions)[] { m_forwardDirection, m_rightDirection };
		directions[!m_isLeftHandedCoordinateSystem].Cross(directions[m_isLeftHandedCoordinateSystem]).Normalize(m_upDirection);
		m_view = m_isLeftHandedCoordinateSystem ?
			DirectX::XMMatrixLookAtLH(m_position, m_position + m_forwardDirection, m_upDirection) :
			DirectX::XMMatrixLookAtRH(m_position, m_position + m_forwardDirection, m_upDirection);
		m_viewDirty = false;
	}

	auto GetView() const { return m_view; }

	auto GetProjection() const { return m_projection; }

	void Translate(const DirectX::XMFLOAT3& displacement) {
		m_position += displacement;
		m_viewDirty = true;
	}

	void Yaw(float angle) {
		using namespace DirectX::SimpleMath;
		const auto rotation = Matrix::CreateRotationY(angle);
		m_rightDirection = Vector3::TransformNormal(m_rightDirection, rotation);
		m_forwardDirection = Vector3::TransformNormal(m_forwardDirection, rotation);
		m_viewDirty = true;
	}

	void Pitch(float angle) {
		using namespace DirectX::SimpleMath;
		const auto rotation = Matrix::CreateFromAxisAngle(m_rightDirection, angle);
		m_upDirection = Vector3::TransformNormal(m_upDirection, rotation);
		m_forwardDirection = Vector3::TransformNormal(m_forwardDirection, rotation);
		m_viewDirty = true;
	}

private:
	bool m_isLeftHandedCoordinateSystem;

	DirectX::SimpleMath::Vector3 m_position, m_forwardDirection, m_upDirection{ 0, 1, 0 }, m_rightDirection{ 1, 0, 0 };

	float m_fovAngleY{}, m_aspectRatio{}, m_nearZ{}, m_farZ{};

	bool m_viewDirty = true;
	DirectX::XMMATRIX m_view{}, m_projection{};
};
