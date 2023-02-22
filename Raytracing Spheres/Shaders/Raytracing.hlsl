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

	float viewZ = 1.#INFf;
	float3 motion = 0, irradiance = 0;

	RayCastResult rayCastResult;
	const float2 UV = Math::CalculateUV(raysIndex, raysDimensions, g_camera.PixelJitter);
	const RayDesc rayDesc = g_camera.GeneratePinholeRay(Math::CalculateNDC(UV));
	if (CastRay(rayDesc, rayCastResult)) {
		viewZ = dot(rayCastResult.HitInfo.Vertex.Position - g_camera.Position, normalize(g_camera.ForwardDirection));

		const float3 previousPosition = STL::Geometry::AffineTransform(g_instanceData[rayCastResult.InstanceIndex].WorldToPreviousWorld, rayCastResult.HitInfo.Vertex.Position);
		motion.xy = STL::Geometry::GetScreenUv(g_camera.PreviousWorldToProjection, previousPosition) - UV;
		motion.z = STL::Geometry::AffineTransform(g_camera.PreviousWorldToView, previousPosition).z - viewZ;

		g_normalRoughness[raysIndex] = NRD_FrontEnd_PackNormalAndRoughness(rayCastResult.HitInfo.Vertex.Normal, rayCastResult.Material.Roughness);

		g_baseColorMetalness[raysIndex] = float4(rayCastResult.Material.BaseColor.rgb, rayCastResult.Material.Metallic);

		ScatterResult scatterResult;
		float hitDistance;
		for (uint i = 0; i < g_globalData.SamplesPerPixel; i++) {
			const ScatterResult scatterResultTemp = rayCastResult.Material.Scatter(rayCastResult.HitInfo, rayDesc.Direction);
			const IndirectRay::TraceResult traceResult = IndirectRay::Trace(rayCastResult.HitInfo.Vertex.Position, scatterResultTemp.Direction);
			if (!i) {
				scatterResult = scatterResultTemp;
				hitDistance = traceResult.HitDistance;
			}
			irradiance += traceResult.Irradiance;
		}

		irradiance *= NRD_IsValidRadiance(irradiance) ? 1.0f / g_globalData.SamplesPerPixel : 0;
		irradiance = rayCastResult.Material.EmissiveColor.rgb + (g_globalData.MaxTraceRecursionDepth ? irradiance * scatterResult.Attenuation : rayCastResult.Material.BaseColor.rgb);

		const bool isDiffuse = scatterResult.Type == ScatterType::DiffuseReflection;

		hitDistance = REBLUR_FrontEnd_GetNormHitDist(hitDistance, viewZ, g_globalData.NRDHitDistanceParameters, isDiffuse ? 1 : rayCastResult.Material.Roughness);

		float3 albedo, Rf0;
		STL::BRDF::ConvertBaseColorMetalnessToAlbedoRf0(rayCastResult.Material.BaseColor.rgb, rayCastResult.Material.Metallic, albedo, Rf0);

		const float3 Fenvironment = STL::BRDF::EnvironmentTerm_Rtg(Rf0, abs(dot(rayCastResult.HitInfo.Vertex.Normal, -rayDesc.Direction)), rayCastResult.Material.Roughness);
		const float4 radiance = REBLUR_FrontEnd_PackRadianceAndNormHitDist(irradiance / (isDiffuse ? (1 - Fenvironment) * albedo * 0.99f + 0.01f : Fenvironment * 0.99f + 0.01f), hitDistance);
		g_noisyDiffuse[raysIndex] = isDiffuse ? radiance : 0;
		g_noisySpecular[raysIndex] = isDiffuse ? 0 : radiance;
	}
	else if (!GetEnvironmentColor(rayDesc.Direction, irradiance)) irradiance = GetEnvironmentLightColor(rayDesc.Direction);

	g_viewZ[raysIndex] = viewZ;
	g_motion[raysIndex] = motion;
	g_output[raysIndex] = float4(irradiance, viewZ * NRD_FP16_VIEWZ_SCALE);
}
