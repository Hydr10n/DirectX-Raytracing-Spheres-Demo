#pragma once

#include "Raytracing.hlsli"

#include "RaytracingHelpers.hlsli"

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

		for (uint bounce = 0; bounce < g_graphicsSettings.MaxNumberOfBounces; bounce++) {
			const RayDesc rayDesc = { RaytracingHelpers::OffsetRay(hitInfo.Position, hitInfo.Normal * STL::Math::Sign(dot(scatterResult.Direction, hitInfo.Normal))), 0,scatterResult.Direction, 1.#INFf };
			if (CastRay(rayDesc, hitInfo)) {
				const Material material = GetMaterial(hitInfo.ObjectIndex, hitInfo.TextureCoordinate);

				scatterResult = material.Scatter(hitInfo, rayDesc.Direction);

				emissiveColor += throughput * material.EmissiveColor.rgb;
				throughput *= scatterResult.Throughput;

				if (!bounce) traceResult.HitDistance = hitInfo.Distance;

				if (g_graphicsSettings.IsRussianRouletteEnabled) {
					if (bounce > 2) {
						const float probability = max(throughput.r, max(throughput.g, throughput.b));
						if (STL::Rng::Hash::GetFloat() >= probability) break;
						throughput /= probability;
					}
				}
				else if (STL::Color::Luminance(throughput) < 1e-3f) break;
			}
			else {
				incidentColor = GetEnvironmentLightColor(rayDesc.Direction);
				break;
			}
		}

		traceResult.Radiance = emissiveColor + incidentColor * throughput;

		return traceResult;
	}
};
