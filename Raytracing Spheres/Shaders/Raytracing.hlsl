#include "RadianceRay.hlsli"

[shader("raygeneration")]
void RayGeneration() {
	const uint2 raysIndex = DispatchRaysIndex().xy, raysDimensions = DispatchRaysDimensions().xy;

	Random random;
	random.Initialize(raysIndex.x + raysIndex.y * raysDimensions.x, g_sceneConstant.FrameCount);

	float4 color = 0;
	for (uint i = 0; i < g_sceneConstant.RaytracingSamplesPerPixel; i++) {
		const RayDesc ray = GenerateCameraRay(raysIndex, raysDimensions, g_sceneConstant.CameraPosition, g_sceneConstant.ProjectionToWorld, random.Float2());
		color += TraceRadianceRay(ray, MAX_TRACE_RECURSION_DEPTH, random);
	}

	g_output[raysIndex] = color / g_sceneConstant.RaytracingSamplesPerPixel;
}
