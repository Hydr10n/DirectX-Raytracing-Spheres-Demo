#include "PrimaryRay.hlsli"

RWTexture2D<float4> g_output : register(u0);

[shader("raygeneration")]
void RayGeneration() {
	const uint3 raysIndex = DispatchRaysIndex(), raysDimensions = DispatchRaysDimensions();

	Random random;
	random.Initialize(raysIndex.x + raysIndex.y * raysDimensions.x, g_sceneConstant.FrameCount);

	float4 color = 0;
	for (uint i = 0; i < g_sceneConstant.AntiAliasingSampleCount; i++) {
		color += TracePrimaryRay(GenerateCameraRay(raysIndex, raysDimensions, g_sceneConstant.ProjectionToWorld, random.Float2(-0.5, 0.5)), g_sceneConstant.MaxTraceRecursionDepth, random);
	}
	g_output[raysIndex.xy] = color / g_sceneConstant.AntiAliasingSampleCount;
}
