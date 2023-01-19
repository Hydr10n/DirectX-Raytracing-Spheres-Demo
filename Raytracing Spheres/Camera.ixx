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
	};

	struct CameraController {
		const auto& GetPosition() const { return m_position; }

		void SetPosition(const XMFLOAT3& position) {
			m_position = position;

			m_isViewChanged = true;
		}

		const auto& GetRightDirection() const { return m_rightDirection; }
		const auto& GetUpDirection() const { return m_upDirection; }
		const auto& GetForwardDirection() const { return m_forwardDirection; }

		const auto GetNormalizedRightDirection() const { return m_rightDirection / m_rightDirection.Length(); }
		const auto GetNormalizedUpDirection() const { return m_upDirection / m_upDirection.Length(); }
		const auto GetNormalizedForwardDirection() const { return m_forwardDirection / m_forwardDirection.Length(); }

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

			m_isViewChanged = true;
		}

		const auto& GetRotation() const { return m_rotation; }

		void SetRotation(const XMFLOAT3& rotation) {
			m_rotation = { remainder(rotation.x, XM_2PI), remainder(rotation.y, XM_2PI), remainder(rotation.z, XM_2PI) };

			const auto transform = Quaternion::CreateFromYawPitchRoll(m_rotation.y, -m_rotation.x, -m_rotation.z);
			Vector3::Transform({ 1, 0, 0 }, transform).Normalize(m_rightDirection);
			Vector3::Transform({ 0, 0, 1 }, transform).Normalize(m_forwardDirection);

			m_upDirection = m_forwardDirection.Cross(m_rightDirection) * m_upDirectionLength;
			m_rightDirection *= m_rightDirectionLength;
			m_forwardDirection *= m_forwardDirectionLength;

			m_isViewChanged = true;
		}

		void LookAt(const XMFLOAT3& origin, const XMFLOAT3& focusPosition, const XMFLOAT3& upDirection = { 0, 1, 0 }, bool setFocusDistance = true) {
			SetDirections(focusPosition - origin, upDirection, setFocusDistance);
		}

		auto GetFocusDistance() const { return m_forwardDirectionLength; }

		void SetFocusDistance(float length) {
			m_rightDirectionLength *= length / m_forwardDirectionLength;
			m_upDirectionLength *= length / m_forwardDirectionLength;
			m_forwardDirectionLength = length;
			m_rightDirection = GetNormalizedRightDirection() * m_rightDirectionLength;
			m_upDirection = GetNormalizedUpDirection() * m_upDirectionLength;
			m_forwardDirection = GetNormalizedForwardDirection() * m_forwardDirectionLength;
		}

		const auto& GetView() const {
			if (m_isViewChanged) {
				m_view = XMMatrixLookToLH(m_position, m_forwardDirection, m_upDirection);

				m_isViewChanged = false;
			}
			return m_view;
		}

		void Translate(const XMFLOAT3& displacement) { SetPosition(m_position + displacement); }

		void Rotate(const XMFLOAT3& rotation) { SetRotation(m_rotation + rotation); }

		auto GetNearZ() const { return m_nearZ; }

		auto GetFarZ() const { return m_farZ; }

		const auto& GetProjection() const { return m_projection; }

		void SetLens(float verticalFieldOfView, float aspectRatio, float nearZ = 1e-2f, float farZ = 1e3f) {
			m_projection = XMMatrixPerspectiveFovLH(verticalFieldOfView, aspectRatio, nearZ, farZ);
			m_nearZ = nearZ;
			m_farZ = farZ;

			m_upDirectionLength = tan(verticalFieldOfView / 2) * m_forwardDirectionLength;
			m_rightDirectionLength = m_upDirectionLength * aspectRatio;
			m_upDirection = GetNormalizedUpDirection() * m_upDirectionLength;
			m_rightDirection = GetNormalizedRightDirection() * m_rightDirectionLength;
		}

		auto GetViewProjection() const { return GetView() * GetProjection(); }

		auto GetInverseViewProjection() const { return XMMatrixInverse(nullptr, GetViewProjection()); }

	private:
		mutable bool m_isViewChanged = true;
		float m_rightDirectionLength = 1, m_upDirectionLength = 1, m_forwardDirectionLength = 1;
		Vector3 m_position, m_rightDirection{ 1, 0, 0 }, m_upDirection{ 0, 1, 0 }, m_forwardDirection{ 0, 0, 1 }, m_rotation;
		mutable XMMATRIX m_view{};

		float m_nearZ = 1e-2f, m_farZ = 1e3f;
		XMMATRIX m_projection{};
	};
}
