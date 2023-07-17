module;

#include "directxtk12/SimpleMath.h"

export module Camera;

using namespace DirectX;
using namespace DirectX::SimpleMath;

export {
	struct Camera {
		XMFLOAT3 Position;
		float _;
		XMFLOAT3 RightDirection;
		float _1;
		XMFLOAT3 UpDirection;
		float _2;
		XMFLOAT3 ForwardDirection;
		float ApertureRadius;
		XMFLOAT2 PixelJitter;
		float NearZ, FarZ;
		XMFLOAT4X4 PreviousWorldToView, PreviousWorldToProjection;
	};

	struct CameraController {
		const auto& GetPosition() const { return m_position; }

		void SetPosition(const XMFLOAT3& value) {
			m_position = value;

			m_isViewChanged = true;
		}

		const auto& GetRightDirection() const { return m_rightDirection; }
		const auto& GetUpDirection() const { return m_upDirection; }
		const auto& GetForwardDirection() const { return m_forwardDirection; }

		auto GetNormalizedRightDirection() const { return m_rightDirection / m_rightDirection.Length(); }
		auto GetNormalizedUpDirection() const { return m_upDirection / m_upDirection.Length(); }
		auto GetNormalizedForwardDirection() const { return m_forwardDirection / m_forwardDirection.Length(); }

		void SetDirections(const XMFLOAT3& forwardDirection, const XMFLOAT3& upDirection = { 0, 1, 0 }, bool setFocusDistance = true) {
			m_forwardDirection = forwardDirection;
			m_rightDirection = Vector3(upDirection).Cross(forwardDirection);
			m_upDirection = m_forwardDirection.Cross(m_rightDirection);
			if (setFocusDistance) SetFocusDistance(m_forwardDirection.Length());
			else {
				m_rightDirection = GetNormalizedRightDirection() * m_rightDirectionLength;
				m_upDirection = GetNormalizedUpDirection() * m_upDirectionLength;
				m_forwardDirection = GetNormalizedForwardDirection() * m_forwardDirectionLength;
			}

			m_rotation = Quaternion::LookRotation(m_forwardDirection, m_upDirection);

			m_isViewChanged = true;
		}

		const auto& GetRotation() const { return m_rotation; }

		void SetRotation(const Quaternion& value) {
			value.Normalize(m_rotation);

			Vector3::Transform({ 0, 0, 1 }, m_rotation).Normalize(m_forwardDirection);
			Vector3::Transform({ 1, 0, 0 }, m_rotation).Normalize(m_rightDirection);

			m_upDirection = m_forwardDirection.Cross(m_rightDirection) * m_upDirectionLength;
			m_forwardDirection *= m_forwardDirectionLength;
			m_rightDirection *= m_rightDirectionLength;

			m_isViewChanged = true;
		}

		void LookAt(const XMFLOAT3& position, const XMFLOAT3& upDirection = { 0, 1, 0 }, bool setFocusDistance = true) {
			SetDirections(position - m_position, upDirection, setFocusDistance);
		}

		auto GetFocusDistance() const { return m_forwardDirectionLength; }

		void SetFocusDistance(float value) {
			m_rightDirectionLength *= value / m_forwardDirectionLength;
			m_upDirectionLength *= value / m_forwardDirectionLength;
			m_forwardDirectionLength = value;
			m_rightDirection = GetNormalizedRightDirection() * m_rightDirectionLength;
			m_upDirection = GetNormalizedUpDirection() * m_upDirectionLength;
			m_forwardDirection = GetNormalizedForwardDirection() * m_forwardDirectionLength;
		}

		void Move(const XMFLOAT3& value) { SetPosition(m_position + value); }

		void Rotate(float yaw, float pitch, float roll = 0) {
			m_rotation *= Quaternion::CreateFromAxisAngle(m_rightDirection, -pitch);
			m_rotation *= Quaternion::CreateFromAxisAngle({ 0, 1, 0 }, yaw);
			m_rotation *= Quaternion::CreateFromAxisAngle(m_forwardDirection, -roll);
			SetRotation(m_rotation);
		}

		const auto& GetWorldToView() const {
			if (m_isViewChanged) {
				m_worldToView = XMMatrixLookToLH(m_position, m_forwardDirection, m_upDirection);

				m_isViewChanged = false;
			}
			return m_worldToView;
		}

		auto GetViewToWorld() const { return GetWorldToView().Transpose(); }

		auto GetNearZ() const { return m_nearZ; }
		auto GetFarZ() const { return m_farZ; }

		const auto& GetViewToProjection() const { return m_viewToProjection; }
		auto GetProjectionToView() const { return m_viewToProjection.Invert(); }

		auto GetVerticalFieldOfView() const { return m_verticalFieldOfView; }
		auto GetAspectRatio() const { return m_aspectRatio; }

		void SetLens(float verticalFieldOfView, float aspectRatio, float nearZ, float farZ) {
			m_viewToProjection = XMMatrixPerspectiveFovLH(verticalFieldOfView, aspectRatio, nearZ, farZ);

			m_verticalFieldOfView = verticalFieldOfView;
			m_aspectRatio = aspectRatio;

			m_nearZ = nearZ;
			m_farZ = farZ;

			m_upDirectionLength = tan(verticalFieldOfView / 2) * m_forwardDirectionLength;
			m_rightDirectionLength = m_upDirectionLength * aspectRatio;
			m_upDirection = GetNormalizedUpDirection() * m_upDirectionLength;
			m_rightDirection = GetNormalizedRightDirection() * m_rightDirectionLength;
		}

		void SetLens(float verticalFieldOfView, float aspectRatio) { SetLens(verticalFieldOfView, aspectRatio, m_nearZ, m_farZ); }

		auto GetWorldToProjection() const { return GetWorldToView() * GetViewToProjection(); }
		auto GetProjectionToWorld() const { return GetWorldToProjection().Invert(); }

	private:
		mutable bool m_isViewChanged = true;
		float m_rightDirectionLength = 1, m_upDirectionLength = 1, m_forwardDirectionLength = 1;
		Vector3 m_position, m_rightDirection{ 1, 0, 0 }, m_upDirection{ 0, 1, 0 }, m_forwardDirection{ 0, 0, 1 };
		Quaternion m_rotation;
		mutable Matrix m_worldToView;

		float m_verticalFieldOfView{}, m_aspectRatio{};
		float m_nearZ = 1e-2f, m_farZ = 1e4f;
		Matrix m_viewToProjection;
	};
}
