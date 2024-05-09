#pragma once

#include "Raytracing.hlsli"

struct IndirectRay {
	struct TraceResult {
		float3 Radiance;
		float HitDistance;
	};

	static TraceResult Trace(HitInfo hitInfo, float3 worldRayDirection) {
		TraceResult traceResult;
		traceResult.HitDistance = 1.#INFf;

		float3 emissiveColor = 0, incidentColor = 0, throughput = 1;

		ScatterResult scatterResult;
		scatterResult.Direction = worldRayDirection;

		for (uint bounce = 1; bounce <= g_graphicsSettings.Bounces; bounce++) {
			const RayDesc rayDesc = { hitInfo.GetSafeWorldRayOrigin(scatterResult.Direction), 0, scatterResult.Direction, 1.#INFf };
			if (!CastRay(rayDesc, hitInfo)) {
				incidentColor = GetEnvironmentLightColor(rayDesc.Direction);
				break;
			}

			const Material material = GetMaterial(hitInfo.ObjectIndex, hitInfo.TextureCoordinate);

			scatterResult = material.Scatter(hitInfo, rayDesc.Direction);

			emissiveColor += throughput * material.EmissiveColor;
			throughput *= scatterResult.Throughput;

			if (bounce == 1) traceResult.HitDistance = hitInfo.Distance;

			if (g_graphicsSettings.IsRussianRouletteEnabled) {
				if (bounce > 3) {
					const float probability = max(throughput.r, max(throughput.g, throughput.b));
					if (STL::Rng::Hash::GetFloat() >= probability) break;
					throughput /= probability;
				}
			}
			else if (STL::Color::Luminance(throughput) < 1e-3f) break;
		}

		traceResult.Radiance = emissiveColor + incidentColor * throughput;

		return traceResult;
	}
};
