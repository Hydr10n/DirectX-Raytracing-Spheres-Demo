module;

#include "directxtk12/SimpleMath.h"

export module Camera;

using namespace DirectX;
using namespace DirectX::SimpleMath;
using namespace std;

export {
	struct Camera {
		XMFLOAT3 Position;
		float _;
		XMFLOAT2 Jitter;
		XMFLOAT2 _1;
		XMMATRIX ProjectionToWorld;
	};

	struct FirstPersonCamera {
		const auto& GetPosition() const { return m_position; }

		void SetPosition(const XMFLOAT3& position) {
			m_position = position;

			m_isViewChanged = true;
		}

		const auto& GetRightDirection() const { return m_rightDirection; }

		const auto& GetUpDirection() const { return m_upDirection; }

		const auto& GetForwardDirection() const { return m_forwardDirection; }

		void SetDirections(const XMFLOAT3& up, const XMFLOAT3& forward) {
			(m_forwardDirection = forward).Normalize();
			Vector3(up).Cross(forward).Normalize(m_rightDirection);
			m_upDirection = m_forwardDirection.Cross(m_rightDirection);

			m_isViewChanged = true;
		}

		const auto& GetView() const {
			if (m_isViewChanged) {
				m_forwardDirection.Cross(m_rightDirection).Normalize(m_upDirection);

				m_view = XMMatrixLookToLH(m_position, m_forwardDirection, m_upDirection);

				m_isViewChanged = false;
			}
			return m_view;
		}

		void Translate(const XMFLOAT3& displacement) {
			m_position += displacement;

			m_isViewChanged = true;
		}

		void Yaw(float angle) {
			const auto rotation = Matrix::CreateRotationY(angle);
			Vector3::Transform(m_rightDirection, rotation).Normalize(m_rightDirection);
			Vector3::Transform(m_forwardDirection, rotation).Normalize(m_forwardDirection);

			m_isViewChanged = true;
		}

		void Pitch(float angle) {
			const auto rotation = Matrix::CreateFromAxisAngle(m_rightDirection, angle);
			Vector3::Transform(m_upDirection, rotation).Normalize(m_upDirection);
			Vector3::Transform(m_forwardDirection, rotation).Normalize(m_forwardDirection);

			m_isViewChanged = true;
		}

		auto GetNearZ() const { return m_nearZ; }

		auto GetFarZ() const { return m_farZ; }

		const auto& GetProjection() const { return m_projection; }

		void SetLens(float fovAngleY, float aspectRatio, float nearZ = 1e-1f, float farZ = 1e4f) {
			m_projection = XMMatrixPerspectiveFovLH(fovAngleY, aspectRatio, nearZ, farZ);
			m_nearZ = nearZ;
			m_farZ = farZ;
		}

		auto GetViewProjection() const { return GetView() * GetProjection(); }

		auto InverseViewProjection() const { return XMMatrixInverse(nullptr, GetViewProjection()); }

	private:
		mutable bool m_isViewChanged = true;
		Vector3 m_position;
		mutable Vector3 m_rightDirection{ 1, 0, 0 }, m_upDirection{ 0, 1, 0 }, m_forwardDirection{ 0, 0, 1 };
		mutable XMMATRIX m_view{};

		float m_nearZ{}, m_farZ{};
		XMMATRIX m_projection{};
	};
}
