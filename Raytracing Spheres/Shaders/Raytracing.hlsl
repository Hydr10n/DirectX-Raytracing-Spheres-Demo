#include "RadianceRay.hlsli"

#define ROOT_SIGNATURE \
	"RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
	"StaticSampler(s0)," \
	"SRV(t0)," \
	"CBV(b0)"

[RootSignature(ROOT_SIGNATURE)]
[numthreads(16, 16, 1)]
void main(uint2 raysIndex : SV_DispatchThreadID) {
	uint2 raysDimensions;
	g_output.GetDimensions(raysDimensions.x, raysDimensions.y);
	if (raysIndex.x > raysDimensions.x || raysIndex.y > raysDimensions.y) return;

	Random random;
	random.Initialize(raysIndex.x + raysIndex.y * raysDimensions.x, g_globalData.FrameCount);

	float4 color = 0;
	for (uint i = 0; i < g_globalData.RaytracingSamplesPerPixel; i++) {
		const Ray ray = g_camera.GenerateRay(raysIndex, raysDimensions, random.Float2());
		color += RadianceRay::Trace(ray.ToDesc(), random);
	}
	color = any(isnan(color)) ? 0 : color;
	color /= g_globalData.RaytracingSamplesPerPixel;
	color = (g_globalData.AccumulatedFrameIndex * g_output[raysIndex] + color) / (g_globalData.AccumulatedFrameIndex + 1);
	g_output[raysIndex] = color;
}
