#include "IndirectRay.hlsli"

#define NRD_HEADER_ONLY
#include "NRDEncoding.hlsli"
#include "NRD.hlsli"

[RootSignature(
	"RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED),"
	"StaticSampler(s0),"
	"SRV(t0),"
	"CBV(b0),"
	"CBV(b1),"
	"CBV(b2),"
	"SRV(t1),"
	"SRV(t2),"
	"DescriptorTable(UAV(u0)),"
	"DescriptorTable(UAV(u1)),"
	"DescriptorTable(UAV(u2)),"
	"DescriptorTable(UAV(u3)),"
	"DescriptorTable(UAV(u4)),"
	"DescriptorTable(UAV(u5)),"
	"DescriptorTable(UAV(u6)),"
	"DescriptorTable(UAV(u7)),"
	"DescriptorTable(UAV(u8))"
)]
[numthreads(16, 16, 1)]
void main(uint2 pixelPosition : SV_DispatchThreadID) {
	if (pixelPosition.x >= g_graphicsSettings.RenderSize.x || pixelPosition.y >= g_graphicsSettings.RenderSize.y) return;

	STL::Rng::Hash::Initialize(pixelPosition, g_graphicsSettings.FrameIndex);

	float linearDepth = 1.#INFf, normalizedDepth = !g_camera.IsNormalizedDepthReversed;
	float3 motionVector = 0;
	float4 baseColorMetalness = 0;
	float3 emissiveColor = 0;
	float4 normalRoughness = 0;

	HitInfo hitInfo;
	Material material;
	const float2 UV = Math::CalculateUV(pixelPosition, g_graphicsSettings.RenderSize, g_camera.Jitter);
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

	g_linearDepth[pixelPosition] = linearDepth;
	g_normalizedDepth[pixelPosition] = normalizedDepth;
	g_motionVectors[pixelPosition] = motionVector;
	g_baseColorMetalness[pixelPosition] = baseColorMetalness;
	g_emissiveColor[pixelPosition] = emissiveColor;
	g_normalRoughness[pixelPosition] = normalRoughness;

	float3 radiance = 0;
	float4 noisyDiffuse = 0, noisySpecular = 0;

	if (hit) {
		const float NoV = abs(dot(hitInfo.Normal, V));

		bool isDiffuse = true;
		float hitDistance = 1.#INFf;
		const float bayer4x4 = STL::Sequence::Bayer4x4(pixelPosition, g_graphicsSettings.FrameIndex);
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

		const NRDSettings NRDSettings = g_graphicsSettings.NRD;
		if (NRDSettings.Denoiser != NRDDenoiser::None) {
			float3 albedo, Rf0;
			STL::BRDF::ConvertBaseColorMetalnessToAlbedoRf0(material.BaseColor.rgb, material.Metallic, albedo, Rf0);
			const float3 Fenvironment = STL::BRDF::EnvironmentTerm_Rtg(Rf0, NoV, material.Roughness);
			float4 radianceHitDistance = 0;
			if (NRDSettings.Denoiser == NRDDenoiser::ReBLUR) {
				const float normalizedHitDistance = REBLUR_FrontEnd_GetNormHitDist(hitDistance, linearDepth, NRDSettings.HitDistanceParameters, isDiffuse ? 1 : material.Roughness);
				radianceHitDistance = REBLUR_FrontEnd_PackRadianceAndNormHitDist(radiance / lerp(isDiffuse ? (1 - Fenvironment) * albedo : Fenvironment, 1, 0.01f), normalizedHitDistance);
			}
			else if (NRDSettings.Denoiser == NRDDenoiser::ReLAX) {
				radianceHitDistance = RELAX_FrontEnd_PackRadianceAndHitDist(radiance / lerp(isDiffuse ? (1 - Fenvironment) * albedo : Fenvironment, 1, 0.01f), hitDistance);
			}
			if (isDiffuse) noisyDiffuse = radianceHitDistance;
			else noisySpecular = radianceHitDistance;
		}

		radiance += material.EmissiveColor;
	}
	else if (!GetEnvironmentColor(rayDesc.Direction, radiance)) radiance = GetEnvironmentLightColor(rayDesc.Direction);

	g_color[pixelPosition] = float4(radiance, linearDepth * NRD_FP16_VIEWZ_SCALE);
	g_noisyDiffuse[pixelPosition] = noisyDiffuse;
	g_noisySpecular[pixelPosition] = noisySpecular;
}
