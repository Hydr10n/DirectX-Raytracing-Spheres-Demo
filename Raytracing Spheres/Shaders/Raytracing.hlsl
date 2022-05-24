#include "RadianceRay.hlsli"

[shader("raygeneration")]
void RayGeneration() {
	const uint2 raysIndex = DispatchRaysIndex().xy, raysDimensions = DispatchRaysDimensions().xy;

	Random random;
	random.Initialize(raysIndex.x + raysIndex.y * raysDimensions.x, g_sceneConstants.FrameCount);

	float4 color = 0;
	for (uint i = 0; i < g_sceneConstants.RaytracingSamplesPerPixel; i++) {
		const RayDesc ray = GenerateCameraRay(raysIndex, raysDimensions, g_sceneConstants.CameraPosition, g_sceneConstants.ProjectionToWorld, random.Float2());
		color += TraceRadianceRay(ray, MAX_TRACE_RECURSION_DEPTH, random);
	}

	g_output[raysIndex] = color / g_sceneConstants.RaytracingSamplesPerPixel;
}
