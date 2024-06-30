module;

#include "directxtk12/SimpleMath.h"

#include "ml.h"

export module Camera;

using namespace DirectX;
using namespace DirectX::SimpleMath;
using namespace std;

export {
	struct Camera {
		BOOL IsNormalizedDepthReversed;
		XMFLOAT3 PreviousPosition, Position;
		float _;
		XMFLOAT3 RightDirection;
		float _1;
		XMFLOAT3 UpDirection;
		float _2;
		XMFLOAT3 ForwardDirection;
		float ApertureRadius, NearDepth, FarDepth;
		XMFLOAT2 Jitter;
		XMFLOAT4X4 PreviousWorldToView, PreviousViewToProjection, PreviousWorldToProjection, PreviousProjectionToView, PreviousViewToWorld, WorldToProjection;
	};

	struct CameraController {
		explicit CameraController(bool isNormalizedDepthReversed = true) : m_projectionFlags(PROJ_LEFT_HANDED | (isNormalizedDepthReversed ? PROJ_REVERSED_Z : 0)) {}

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

		void Translate(const XMFLOAT3& value) { SetPosition(m_position + value); }

		void Rotate(float yaw, float pitch, float roll = 0) {
			SetRotation(m_rotation * Quaternion::CreateFromAxisAngle(m_rightDirection, -pitch) * Quaternion::CreateFromAxisAngle({ 0, 1, 0 }, yaw) * Quaternion::CreateFromAxisAngle(m_forwardDirection, -roll));
		}

		const auto& GetWorldToView() const {
			if (m_isViewChanged) {
				m_worldToView = XMMatrixLookToLH(m_position, m_forwardDirection, m_upDirection);

				m_isViewChanged = false;
			}
			return m_worldToView;
		}

		auto GetViewToWorld() const {
			Matrix viewToWorld(GetNormalizedRightDirection(), GetNormalizedUpDirection(), GetNormalizedForwardDirection());
			reinterpret_cast<Vector3&>(viewToWorld.m[3]) = m_position;
			return viewToWorld;
		}

		auto GetHorizontalFieldOfView() const { return m_horizontalFieldOfView; }
		auto GetVerticalFieldOfView() const { return 2 * atan(tan(m_horizontalFieldOfView / 2) * m_aspectRatio); }
		auto GetAspectRatio() const { return m_aspectRatio; }

		auto GetNearDepth() const { return m_nearDepth; }
		auto GetFarDepth() const { return m_farDepth; }

		const auto& GetViewToProjection() const {
			if (m_isProjectionChanged) {
				float4x4 viewToProjection;
				if (m_farDepth == numeric_limits<float>::infinity()) viewToProjection.SetupByHalfFovxInf(m_horizontalFieldOfView / 2, m_aspectRatio, m_nearDepth, m_projectionFlags);
				else viewToProjection.SetupByHalfFovx(m_horizontalFieldOfView / 2, m_aspectRatio, m_nearDepth, m_farDepth, m_projectionFlags);
				m_viewToProjection = reinterpret_cast<const Matrix&>(viewToProjection);

				m_isProjectionChanged = false;
			}
			return m_viewToProjection;
		}

		auto GetProjectionToView() const { return GetViewToProjection().Invert(); }

		void SetLens(float horizontalFieldOfView, float aspectRatio) {
			m_horizontalFieldOfView = horizontalFieldOfView;
			m_aspectRatio = aspectRatio;

			m_rightDirectionLength = tan(horizontalFieldOfView / 2) * m_forwardDirectionLength;
			m_upDirectionLength = m_rightDirectionLength / aspectRatio;
			m_upDirection = GetNormalizedUpDirection() * m_upDirectionLength;
			m_rightDirection = GetNormalizedRightDirection() * m_rightDirectionLength;

			m_isProjectionChanged = true;
		}

		void SetLens(float horizontalFieldOfView, float aspectRatio, float nearDepth, float farDepth = numeric_limits<float>::infinity()) {
			SetLens(horizontalFieldOfView, aspectRatio);

			m_nearDepth = nearDepth;
			m_farDepth = farDepth;
		}

		auto GetWorldToProjection() const { return GetWorldToView() * GetViewToProjection(); }
		auto GetProjectionToWorld() const { return GetProjectionToView() * GetViewToWorld(); }

	private:
		mutable bool m_isViewChanged = true;
		float m_rightDirectionLength = 1, m_upDirectionLength = 1, m_forwardDirectionLength = 1;
		Vector3 m_position, m_rightDirection{ 1, 0, 0 }, m_upDirection{ 0, 1, 0 }, m_forwardDirection{ 0, 0, 1 };
		Quaternion m_rotation;
		mutable Matrix m_worldToView;

		mutable bool m_isProjectionChanged = true;
		uint32_t m_projectionFlags;
		float m_horizontalFieldOfView{}, m_aspectRatio{}, m_nearDepth = 1e-2f, m_farDepth = numeric_limits<float>::infinity();
		mutable Matrix m_viewToProjection;
	};
}
