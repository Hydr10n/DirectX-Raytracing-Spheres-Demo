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

		depth = dot(hitInfo.Position - g_camera.Position, normalize(g_camera.ForwardDirection));
		motionVector = CalculateMotionVector(UV, depth, hitInfo.Position, hitInfo.ObjectPosition, hitInfo.InstanceIndex, hitInfo.ObjectIndex, hitInfo.PrimitiveIndex, hitInfo.Barycentrics);
		baseColorMetalness = float4(material.BaseColor.rgb, material.Metallic);
		emissiveColor = material.EmissiveColor.rgb;
		normalRoughness = NRD_FrontEnd_PackNormalAndRoughness(hitInfo.Normal, material.Roughness, material.EstimateDiffuseProbability(hitInfo.Normal, V) != 0);
	}

	g_depth[pixelCoordinate] = depth;
	g_motionVectors[pixelCoordinate] = motionVector;
	g_baseColorMetalness[pixelCoordinate] = baseColorMetalness;
	g_emissiveColor[pixelCoordinate] = emissiveColor;
	g_normalRoughness[pixelCoordinate] = normalRoughness;

	float3 radiance = 0;

	if (hit) {
		const float NoV = abs(dot(hitInfo.Normal, V));

		hitInfo.Position = RaytracingHelpers::OffsetRay(hitInfo.Position, hitInfo.Normal) + (V + hitInfo.Normal * STL::BRDF::Pow5(NoV)) * (3e-4f + depth * 5e-5f);

		ScatterResult scatterResult;
		float hitDistance;
		const float bayer4x4 = STL::Sequence::Bayer4x4(pixelCoordinate, g_graphicsSettings.FrameIndex);
		for (uint i = 0; i < g_graphicsSettings.SamplesPerPixel; i++) {
			const ScatterResult scatterResultTemp = material.Scatter(hitInfo, rayDesc.Direction, bayer4x4 + STL::Rng::Hash::GetFloat() / 16);
			const IndirectRay::TraceResult traceResult = IndirectRay::Trace(hitInfo, scatterResultTemp.Direction);
			if (!i) {
				scatterResult = scatterResultTemp;
				hitDistance = traceResult.HitDistance;
			}
			radiance += traceResult.Radiance * scatterResultTemp.Throughput;
		}

		radiance *= NRD_IsValidRadiance(radiance) ? 1.0f / g_graphicsSettings.SamplesPerPixel : 0;

		const bool isDiffuse = scatterResult.Type == ScatterType::DiffuseReflection;

		hitDistance = REBLUR_FrontEnd_GetNormHitDist(hitDistance, depth, g_graphicsSettings.NRDHitDistanceParameters, isDiffuse ? 1 : material.Roughness);

		float3 albedo, Rf0;
		STL::BRDF::ConvertBaseColorMetalnessToAlbedoRf0(material.BaseColor.rgb, material.Metallic, albedo, Rf0);
		const float3 Fenvironment = STL::BRDF::EnvironmentTerm_Rtg(Rf0, NoV, material.Roughness);
		const float4 radianceHitDistance = REBLUR_FrontEnd_PackRadianceAndNormHitDist(radiance / lerp(isDiffuse ? (1 - Fenvironment) * albedo : Fenvironment, 1, 0.01f), hitDistance);
		g_noisyDiffuse[pixelCoordinate] = isDiffuse ? radianceHitDistance : 0;
		g_noisySpecular[pixelCoordinate] = isDiffuse ? 0 : radianceHitDistance;

		radiance += material.EmissiveColor.rgb;
	}
	else if (!GetEnvironmentColor(rayDesc.Direction, radiance)) radiance = GetEnvironmentLightColor(rayDesc.Direction);

	g_output[pixelCoordinate] = float4(radiance, depth * NRD_FP16_VIEWZ_SCALE);
}
