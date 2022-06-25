#include "RadianceRay.hlsli"

[shader("raygeneration")]
void RayGeneration() {
	const uint2 raysIndex = DispatchRaysIndex().xy, raysDimensions = DispatchRaysDimensions().xy;

	Random random;
	random.Initialize(raysIndex.x + raysIndex.y * raysDimensions.x, g_globalData.FrameCount);

	float4 color = 0;
	for (uint i = 0; i < g_globalData.RaytracingSamplesPerPixel; i++) {
		const Ray ray = g_camera.GenerateRay(raysIndex, raysDimensions, random.Float2());
		color += TraceRadianceRay(ray.ToRayDesc(), 0, random);
	}

	g_output[raysIndex] = color / g_globalData.RaytracingSamplesPerPixel;
}
