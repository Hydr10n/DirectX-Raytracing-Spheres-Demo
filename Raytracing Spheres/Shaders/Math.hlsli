#pragma once

namespace Math {
	inline float2 CalculateUV(uint2 pixelCoordinate, uint2 pixelDimensions, float2 pixelJitter = 0) { return (pixelCoordinate + 0.5f + pixelJitter) / pixelDimensions; }

	inline float2 CalculateNDC(float2 UV) { return UV * float2(2, -2) + float2(-1, 1); }

	inline float3 CalculateWorldPosition(float2 NDC, float depth, float3 cameraPosition, float3 cameraRightDirection, float3 cameraUpDirection, float3 cameraForwardDirection) {
		const float3 direction = normalize(NDC.x * cameraRightDirection + NDC.y * cameraUpDirection + cameraForwardDirection);
		return cameraPosition + direction * depth / dot(direction, normalize(cameraForwardDirection));
	}

	inline float3 CalculateTangent(float3 positions[3], float2 textureCoordinates[3]) {
		const float2 d0 = textureCoordinates[1] - textureCoordinates[0], d1 = textureCoordinates[2] - textureCoordinates[0];
		return normalize(((positions[1] - positions[0]) * d1.y - (positions[2] - positions[0]) * d0.y) / (d0.x * d1.y - d0.y * d1.x));
	}
}
