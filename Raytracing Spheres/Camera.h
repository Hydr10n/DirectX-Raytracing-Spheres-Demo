#pragma once

#include <SimpleMath.h>

class Camera {
public:
	float Yaw{}, Pitch{};

	DirectX::SimpleMath::Vector3 Position, UpDirection;

	Camera(const DirectX::XMFLOAT3& position = {}, const DirectX::XMFLOAT3 upDirection = { 0, 1, 0 }) :
		Position(position), UpDirection(upDirection) {}

	void SetLens(float fovAngleY, float aspectRatio, float nearZ, float farZ) {
		m_fovAngleY = fovAngleY;
		m_aspectRatio = aspectRatio;
		m_nearZ = nearZ;
		m_farZ = farZ;
		m_projection = DirectX::XMMatrixPerspectiveFovLH(fovAngleY, aspectRatio, nearZ, farZ);
	}

	auto GetFovAngleY() const { return m_fovAngleY; }

	auto GetAspectRatio() const { return m_aspectRatio; }

	auto GetNearZ() const { return m_nearZ; }

	auto GetFarZ() const { return m_farZ; }

	void Translate(const DirectX::XMFLOAT3& displacement) {
		const auto forwardDirection = GetForwardDirection();
		auto rightDirection = UpDirection.Cross(forwardDirection);
		rightDirection.Normalize();
		Position += rightDirection * displacement.x + UpDirection * displacement.y + forwardDirection * displacement.z;
	}

	void UpdateView() { m_view = DirectX::XMMatrixLookAtLH(Position, Position + GetForwardDirection(), UpDirection); }

	auto GetView() const { return m_view; }

	auto GetProjection() const { return m_projection; }

private:
	float m_fovAngleY{}, m_aspectRatio{}, m_nearZ{}, m_farZ{};

	DirectX::XMMATRIX m_view{}, m_projection{};

	DirectX::SimpleMath::Vector3 GetForwardDirection() const {
		const auto r = cos(Pitch);
		return { r * sin(Yaw), sin(Pitch), r * cos(Yaw) };
	}
};
