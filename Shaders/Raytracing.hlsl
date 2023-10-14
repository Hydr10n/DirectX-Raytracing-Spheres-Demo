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

	float linearDepth = 1.#INFf, normalizedDepth = !g_camera.IsNormalizedDepthReversed;
	float3 motionVector = 0;
	float4 baseColorMetalness = 0;
	float3 emissiveColor = 0;
	float4 normalRoughness = 0;

	HitInfo hitInfo;
	Material material;
	const float2 UV = Math::CalculateUV(pixelCoordinate, g_renderSize, g_camera.PixelJitter);
	const RayDesc rayDesc = g_camera.GeneratePinholeRay(Math::CalculateNDC(UV));
	const float3 V = -rayDesc.Direction;
	const bool hit = CastRay(rayDesc, hitInfo);
	if (hit) {
		material = GetMaterial(hitInfo.ObjectIndex, hitInfo.TextureCoordinate);

		linearDepth = dot(hitInfo.Position - g_camera.Position, normalize(g_camera.ForwardDirection));
		const float4 projection = STL::Geometry::ProjectiveTransform(g_camera.WorldToProjection, hitInfo.Position);
		normalizedDepth = projection.z / projection.w;
		motionVector = CalculateMotionVector(UV, linearDepth, hitInfo);
		baseColorMetalness = float4(material.BaseColor.rgb, material.Metallic);
		emissiveColor = material.EmissiveColor;
		normalRoughness = NRD_FrontEnd_PackNormalAndRoughness(hitInfo.Normal, material.Roughness, material.EstimateDiffuseProbability(hitInfo.Normal, V) == 0);
	}

	g_linearDepth[pixelCoordinate] = linearDepth;
	g_normalizedDepth[pixelCoordinate] = normalizedDepth;
	g_motionVectors[pixelCoordinate] = motionVector;
	g_baseColorMetalness[pixelCoordinate] = baseColorMetalness;
	g_emissiveColor[pixelCoordinate] = emissiveColor;
	g_normalRoughness[pixelCoordinate] = normalRoughness;

	float3 radiance = 0;
	float4 noisyDiffuse = 0, noisySpecular = 0;

	if (hit) {
		const float NoV = abs(dot(hitInfo.Normal, V));

		bool isDiffuse = true;
		float hitDistance = 1.#INFf;
		const float bayer4x4 = STL::Sequence::Bayer4x4(pixelCoordinate, g_graphicsSettings.FrameIndex);
		for (uint i = 0; i < g_graphicsSettings.SamplesPerPixel; i++) {
			const ScatterResult scatterResult = material.Scatter(hitInfo, rayDesc.Direction, bayer4x4 + STL::Rng::Hash::GetFloat() / 16);
			const IndirectRay::TraceResult traceResult = IndirectRay::Trace(hitInfo, scatterResult.Direction);
			if (!i) {
				isDiffuse = scatterResult.Type == ScatterType::DiffuseReflection;
				hitDistance = traceResult.HitDistance;
			}
			radiance += traceResult.Radiance * scatterResult.Throughput;
		}

		radiance *= NRD_IsValidRadiance(radiance) ? 1.0f / g_graphicsSettings.SamplesPerPixel : 0;

		hitDistance = REBLUR_FrontEnd_GetNormHitDist(hitDistance, linearDepth, g_graphicsSettings.NRDHitDistanceParameters, isDiffuse ? 1 : material.Roughness);

		float3 albedo, Rf0;
		STL::BRDF::ConvertBaseColorMetalnessToAlbedoRf0(material.BaseColor.rgb, material.Metallic, albedo, Rf0);
		const float3 Fenvironment = STL::BRDF::EnvironmentTerm_Rtg(Rf0, NoV, material.Roughness);
		const float4 radianceHitDistance = REBLUR_FrontEnd_PackRadianceAndNormHitDist(radiance / lerp(isDiffuse ? (1 - Fenvironment) * albedo : Fenvironment, 1, 0.01f), hitDistance);
		if (isDiffuse) noisyDiffuse = radianceHitDistance;
		else noisySpecular = radianceHitDistance;

		radiance += material.EmissiveColor;
	}
	else if (!GetEnvironmentColor(rayDesc.Direction, radiance)) radiance = GetEnvironmentLightColor(rayDesc.Direction);

	g_output[pixelCoordinate] = float4(radiance, linearDepth * NRD_FP16_VIEWZ_SCALE);
	g_noisyDiffuse[pixelCoordinate] = noisyDiffuse;
	g_noisySpecular[pixelCoordinate] = noisySpecular;
}
