module;

#include "directxtk12/SimpleMath.h"

export module Math;

using namespace DirectX::SimpleMath;

export namespace Math {
	struct int16_t3 { int16_t _[3]{}; };

	struct AffineTransform {
		Vector3 Translation;
		Quaternion Rotation;
		Vector3 Scale{ 1, 1, 1 };

		auto operator()() const {
			return Matrix::CreateScale(Scale) * Matrix::CreateFromQuaternion(Rotation) * Matrix::CreateTranslation(Translation);
		}
	};
}
