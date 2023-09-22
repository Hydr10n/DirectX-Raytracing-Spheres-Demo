#pragma once

namespace RaytracingHelpers {
	/*
	 * Ray Tracing Gems: High-Quality and Real-Time Rendering with DXR and Other APIs, Chapter 6: A Fast and Robust Method for Avoiding Self-Intersection
	 * https://link.springer.com/content/pdf/10.1007/978-1-4842-4427-2_6.pdf
	 */
	inline float3 OffsetRay(float3 position, float3 normal) {
		const float Origin = 1.0f / 32, FloatScale = 1.0f / 65536, IntScale = 256;
		const int3 n = IntScale * normal;
		const float3 p = asfloat(asint(position) + int3(position.x < 0 ? -n.x : n.x, position.y < 0 ? -n.y : n.y, position.z < 0 ? -n.z : n.z));
		return float3(
			abs(position.x) < Origin ? position.x + FloatScale * normal.x : p.x,
			abs(position.y) < Origin ? position.y + FloatScale * normal.y : p.y,
			abs(position.z) < Origin ? position.z + FloatScale * normal.z : p.z
			);
	}
}
