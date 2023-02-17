#include "IndirectRay.hlsli"

#define NRD_HEADER_ONLY
#include "NRDEncoding.hlsli"
#include "NRD.hlsli"

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
	if (raysIndex.x >= raysDimensions.x || raysIndex.y >= raysDimensions.y) return;

	STL::Rng::Initialize(raysIndex, g_globalData.FrameIndex);

	const float2 UV = Math::CalculateUV(raysIndex, raysDimensions, g_camera.PixelJitter), NDC = Math::CalculateNDC(UV);

	const RayDesc rayDesc = g_camera.GeneratePinholeRay(NDC);

	IndirectRay::TraceResult traceResult;
	traceResult.Initialize();
	for (uint i = 0; i < g_globalData.SamplesPerPixel; i++) {
		const IndirectRay::TraceResult temp = IndirectRay::Trace(rayDesc);
		if (!i) traceResult = temp;
		else traceResult.Irradiance += temp.Irradiance;
	}

	const float viewZ = traceResult.InstanceIndex == ~0u ? 0 : dot(traceResult.Vertex.Position - g_camera.Position, normalize(g_camera.ForwardDirection));

	if (traceResult.InstanceIndex == ~0u) g_motion[raysIndex] = 0;
	else {
		const float3 previousPosition = STL::Geometry::AffineTransform(g_instanceData[traceResult.InstanceIndex].WorldToPreviousWorld, traceResult.Vertex.Position);
		float3 motion;
		motion.xy = STL::Geometry::GetScreenUv(g_camera.PreviousWorldToProjection, previousPosition) - UV;
		motion.z = STL::Geometry::AffineTransform(g_camera.PreviousWorldToView, previousPosition).z - viewZ;
		g_motion[raysIndex] = motion;
	}

	g_normalRoughness[raysIndex] = NRD_FrontEnd_PackNormalAndRoughness(traceResult.Vertex.Normal, traceResult.Material.Roughness);

	g_viewZ[raysIndex] = viewZ;

	traceResult.Irradiance *= NRD_IsValidRadiance(traceResult.Irradiance) ? 1.0f / g_globalData.SamplesPerPixel : 0;
	traceResult.HitDistance = REBLUR_FrontEnd_GetNormHitDist(traceResult.HitDistance, viewZ, g_globalData.NRDHitDistanceParameters, traceResult.ScatterType == ScatterType::DiffuseReflection ? 1 : traceResult.Material.Roughness);

	float3 albedo, Rf0;
	STL::BRDF::ConvertBaseColorMetalnessToAlbedoRf0(traceResult.Material.BaseColor, traceResult.Material.Metalness, albedo, Rf0);
	g_baseColorMetalness[raysIndex] = float4(traceResult.Material.BaseColor, traceResult.Material.Metalness);

	const float3 Fenvironment = STL::BRDF::EnvironmentTerm_Rtg(Rf0, abs(dot(traceResult.Vertex.Normal, -rayDesc.Direction)), traceResult.Material.Roughness);
	const float4 radiance = REBLUR_FrontEnd_PackRadianceAndNormHitDist(traceResult.Irradiance / (traceResult.ScatterType == ScatterType::DiffuseReflection ? (1 - Fenvironment) * albedo * 0.99f + 0.01f : Fenvironment * 0.99f + 0.01f), traceResult.HitDistance);
	g_noisyDiffuse[raysIndex] = traceResult.ScatterType == ScatterType::DiffuseReflection ? radiance : 0;
	g_noisySpecular[raysIndex] = traceResult.ScatterType == ScatterType::DiffuseReflection ? 0 : radiance;

	g_output[raysIndex] = float4(traceResult.Irradiance, viewZ * NRD_FP16_VIEWZ_SCALE);
}
