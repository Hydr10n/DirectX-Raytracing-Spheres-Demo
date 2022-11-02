#pragma once

struct Ray {
	float3 Origin, Direction;

	RayDesc ToDesc(float tMin = 1e-3f, float tMax = 1.#INFf) {
		const RayDesc rayDesc = { Origin, tMin, Direction, tMax };
		return rayDesc;
	}
};
