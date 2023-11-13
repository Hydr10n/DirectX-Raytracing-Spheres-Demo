#pragma once

#include "STL.hlsli"

namespace Math {
	inline float2 CalculateUV(uint2 pixelCoordinate, uint2 pixelDimensions, float2 cameraJitter = 0) { return (pixelCoordinate + 0.5f + cameraJitter) / pixelDimensions; }

	inline float2 CalculateNDC(float2 UV) { return UV * float2(2, -2) + float2(-1, 1); }

	inline float3 CalculateWorldPosition(float2 NDC, float linearDepth, float3 cameraPosition, float3 cameraRightDirection, float3 cameraUpDirection, float3 cameraForwardDirection) {
		const float3 direction = normalize(NDC.x * cameraRightDirection + NDC.y * cameraUpDirection + cameraForwardDirection);
		return cameraPosition + direction * linearDepth / dot(direction, normalize(cameraForwardDirection));
	}

	inline float3 CalculateTangent(float3 positions[3], float2 textureCoordinates[3]) {
		const float2 d1 = textureCoordinates[1] - textureCoordinates[0], d2 = textureCoordinates[2] - textureCoordinates[0];
		return ((positions[1] - positions[0]) * d2.y - (positions[2] - positions[0]) * d1.y) / (d1.x * d2.y - d1.y * d2.x);
	}

	inline float2 ToLatLongCoordinate(float3 direction) {
		const float Pi = STL::Math::Pi(1);
		direction = normalize(direction);
		return float2((1 + atan2(direction.x, direction.z) / Pi) / 2, acos(direction.y) / Pi);
	}

	inline float3x3 InverseTranspose(float3x3 m) { return float3x3(cross(m[1], m[2]), cross(m[2], m[0]), cross(m[0], m[1])) / dot(cross(m[0], m[1]), m[2]); }
}
