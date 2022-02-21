#include "PrimaryRay.hlsli"

[shader("raygeneration")]
void RayGeneration() {
	const uint3 raysIndex = DispatchRaysIndex(), raysDimensions = DispatchRaysDimensions();

	Random random;
	random.Initialize(raysIndex.x + raysIndex.y * raysDimensions.x, g_sceneConstant.FrameCount);

	float4 color = 0;
	for (uint i = 0; i < g_sceneConstant.AntiAliasingSampleCount; i++) {
		const RayDesc ray = GenerateCameraRay(raysIndex, raysDimensions, g_sceneConstant.CameraPosition, g_sceneConstant.ProjectionToWorld, random.Float());
		color += TracePrimaryRay(ray, MAX_TRACE_RECURSION_DEPTH, random);
	}

	g_output[raysIndex.xy] = color / g_sceneConstant.AntiAliasingSampleCount;
}
