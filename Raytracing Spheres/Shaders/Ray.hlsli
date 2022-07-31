#pragma once

struct Ray {
	float3 Origin, Direction;

	RayDesc ToDesc(float tMin = 1e-4, float tMax = 1e32) {
		const RayDesc rayDesc = { Origin, tMin, Direction, tMax };
		return rayDesc;
	}
};
