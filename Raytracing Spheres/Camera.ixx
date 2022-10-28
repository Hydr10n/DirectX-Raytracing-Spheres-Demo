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
		XMMATRIX ProjectionToWorld;
	};

	struct FirstPersonCamera {
		const auto& GetPosition() const { return m_position; }

		void SetPosition(const XMFLOAT3& position) {
			m_position = position;

			m_isViewChanged = true;
		}

		const auto& GetDirections() const { return m_directions; }

		void SetDirections(const XMFLOAT3& up, const XMFLOAT3& forward) {
			(m_directions.Forward = forward).Normalize();
			Vector3(up).Cross(forward).Normalize(m_directions.Right);
			m_directions.Up = m_directions.Forward.Cross(m_directions.Right);

			m_isViewChanged = true;
		}

		const auto& GetView() const {
			if (m_isViewChanged) {
				m_directions.Forward.Cross(m_directions.Right).Normalize(m_directions.Up);

				m_view = XMMatrixLookToLH(m_position, m_directions.Forward, m_directions.Up);

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
			Vector3::Transform(m_directions.Right, rotation).Normalize(m_directions.Right);
			Vector3::Transform(m_directions.Forward, rotation).Normalize(m_directions.Forward);

			m_isViewChanged = true;
		}

		void Pitch(float angle) {
			const auto rotation = Matrix::CreateFromAxisAngle(m_directions.Right, angle);
			Vector3::Transform(m_directions.Up, rotation).Normalize(m_directions.Up);
			Vector3::Transform(m_directions.Forward, rotation).Normalize(m_directions.Forward);

			m_isViewChanged = true;
		}

		const auto& GetProjection() const { return m_projection; }

		void SetLens(float fovAngleY, float aspectRatio, float nearZ = 1e-1f, float farZ = 1e4f) { m_projection = XMMatrixPerspectiveFovLH(fovAngleY, aspectRatio, nearZ, farZ); }

		auto GetViewProjection() const { return GetView() * GetProjection(); }

		auto InverseViewProjection() const { return XMMatrixInverse(nullptr, GetViewProjection()); }

	private:
		mutable bool m_isViewChanged{};
		Vector3 m_position;
		mutable struct { Vector3 Right{ 1, 0, 0 }, Up{ 0, 1, 0 }, Forward{ 0, 0, 1 }; } m_directions;
		mutable XMMATRIX m_view{};

		XMMATRIX m_projection{};
	};
}
