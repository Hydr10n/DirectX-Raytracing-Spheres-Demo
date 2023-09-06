#include "IndirectRay.hlsli"

#define NRD_HEADER_ONLY
#include "NRDEncoding.hlsli"
#include "NRD.hlsli"

#define ROOT_SIGNATURE \
	"RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
	"StaticSampler(s0)," \
	"SRV(t0)," \
	"RootConstants(num32BitConstants=2, b0)," \
	"CBV(b1)"

[RootSignature(ROOT_SIGNATURE)]
[numthreads(16, 16, 1)]
void main(uint2 pixelCoordinate : SV_DispatchThreadID) {
	if (pixelCoordinate.x >= g_renderSize.x || pixelCoordinate.y >= g_renderSize.y) return;

	STL::Rng::Hash::Initialize(pixelCoordinate, g_graphicsSettings.FrameIndex);

	float depth = 1.#INFf;
	float3 motionVector = 0, radiance = 0;

	RayCastResult rayCastResult;
	const float2 UV = Math::CalculateUV(pixelCoordinate, g_renderSize, g_camera.PixelJitter);
	const RayDesc rayDesc = g_camera.GeneratePinholeRay(Math::CalculateNDC(UV));
	if (CastRay(rayDesc, rayCastResult)) {
		depth = dot(rayCastResult.HitInfo.Vertex.Position - g_camera.Position, normalize(g_camera.ForwardDirection));

		motionVector = CalculateMotionVector(UV, depth, rayCastResult.HitInfo.Vertex.Position, rayCastResult.HitInfo.ObjectVertexPosition, rayCastResult.InstanceIndex, rayCastResult.ObjectIndex, rayCastResult.PrimitiveIndex, rayCastResult.Barycentrics);

		g_baseColorMetalness[pixelCoordinate] = float4(rayCastResult.Material.BaseColor.rgb, rayCastResult.Material.Metallic);
		g_emissiveColor[pixelCoordinate] = rayCastResult.Material.EmissiveColor.rgb;
		g_normalRoughness[pixelCoordinate] = NRD_FrontEnd_PackNormalAndRoughness(rayCastResult.HitInfo.Vertex.Normal, rayCastResult.Material.Roughness);

		ScatterResult scatterResult;
		float hitDistance;
		for (uint i = 0; i < g_graphicsSettings.SamplesPerPixel; i++) {
			const ScatterResult scatterResultTemp = rayCastResult.Material.Scatter(rayCastResult.HitInfo, rayDesc.Direction);
			const IndirectRay::TraceResult traceResult = IndirectRay::Trace(rayCastResult.HitInfo.Vertex.Position, scatterResultTemp.Direction);
			if (!i) {
				scatterResult = scatterResultTemp;
				hitDistance = traceResult.HitDistance;
			}
			radiance += traceResult.Radiance;
		}

		radiance = NRD_IsValidRadiance(radiance) ? 1.0f / g_graphicsSettings.SamplesPerPixel * radiance * scatterResult.Attenuation : 0;

		const bool isDiffuse = scatterResult.Type == ScatterType::DiffuseReflection;

		hitDistance = REBLUR_FrontEnd_GetNormHitDist(hitDistance, depth, g_graphicsSettings.NRDHitDistanceParameters, isDiffuse ? 1 : rayCastResult.Material.Roughness);

		float3 albedo, Rf0;
		STL::BRDF::ConvertBaseColorMetalnessToAlbedoRf0(rayCastResult.Material.BaseColor.rgb, rayCastResult.Material.Metallic, albedo, Rf0);

		const float3 Fenvironment = STL::BRDF::EnvironmentTerm_Rtg(Rf0, abs(dot(rayCastResult.HitInfo.Vertex.Normal, -rayDesc.Direction)), rayCastResult.Material.Roughness);
		const float4 radianceHitDistance = REBLUR_FrontEnd_PackRadianceAndNormHitDist(radiance / lerp(isDiffuse ? (1 - Fenvironment) * albedo : Fenvironment, 1, 0.01f), hitDistance);
		g_noisyDiffuse[pixelCoordinate] = isDiffuse ? radianceHitDistance : 0;
		g_noisySpecular[pixelCoordinate] = isDiffuse ? 0 : radianceHitDistance;

		radiance += rayCastResult.Material.EmissiveColor.rgb;
	}
	else if (!GetEnvironmentColor(rayDesc.Direction, radiance)) radiance = GetEnvironmentLightColor(rayDesc.Direction);

	g_output[pixelCoordinate] = float4(radiance, depth * NRD_FP16_VIEWZ_SCALE);
	g_depth[pixelCoordinate] = depth;
	g_motionVectors[pixelCoordinate] = motionVector;
}
