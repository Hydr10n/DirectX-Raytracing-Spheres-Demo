module;

#include "directxtk12/SimpleMath.h"

export module Math;

using namespace DirectX::SimpleMath;

export namespace Math {
	struct Transform {
		Vector3 Position;
		Quaternion Rotation;
		Vector3 Scaling{ 1, 1, 1 };

		auto operator()() const { return Matrix::CreateScale(Scaling) * Matrix::CreateFromQuaternion(Rotation) * Matrix::CreateTranslation(Position); }
	};
}
