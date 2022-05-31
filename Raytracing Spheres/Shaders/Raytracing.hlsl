#include "RadianceRay.hlsli"

[shader("raygeneration")]
void RayGeneration() {
	const uint2 raysIndex = DispatchRaysIndex().xy, raysDimensions = DispatchRaysDimensions().xy;

	Random random;
	random.Initialize(raysIndex.x + raysIndex.y * raysDimensions.x, g_sceneConstants.FrameCount);

	float4 color = 0;
	for (uint i = 0; i < g_sceneConstants.RaytracingSamplesPerPixel; i++) {
		const Ray ray = g_sceneConstants.Camera.GenerateRay(raysIndex, raysDimensions, random.Float2());
		color += TraceRadianceRay(ray.ToRayDesc(), MAX_TRACE_RECURSION_DEPTH, random);
	}

	g_output[raysIndex] = color / g_sceneConstants.RaytracingSamplesPerPixel;
}
