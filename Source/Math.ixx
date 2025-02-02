module;

#include "directxtk12/SimpleMath.h"

export module Math;

using namespace DirectX::SimpleMath;

export namespace Math {
	struct uint16_t4 { uint16_t _[4]{}; };

	struct AffineTransform {
		Vector3 Translation;
		Quaternion Rotation;
		Vector3 Scale{ 1, 1, 1 };

		auto operator()() const {
			return Matrix::CreateScale(Scale) * Matrix::CreateFromQuaternion(Rotation) * Matrix::CreateTranslation(Translation);
		}
	};
}
