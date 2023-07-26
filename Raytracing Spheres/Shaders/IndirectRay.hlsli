#pragma once

#include "Raytracing.hlsli"

struct IndirectRay {
	struct TraceResult {
		float3 Radiance;
		float HitDistance;
	};

	static TraceResult Trace(float3 worldRayOrigin, float3 worldRayDirection) {
		TraceResult traceResult;
		traceResult.HitDistance = 1.#INFf;

		float3 emissiveColor = 0, incidentColor = 0, attenuation = 1;

		RayDesc rayDesc;
		rayDesc.Origin = worldRayOrigin;
		rayDesc.Direction = worldRayDirection;
		rayDesc.TMin = 1e-4f;
		rayDesc.TMax = 1.#INFf;

		for (uint depth = 1; depth < g_graphicsSettings.MaxTraceRecursionDepth; depth++) {
			RayCastResult rayCastResult;
			if (CastRay(rayDesc, rayCastResult)) {
				const ScatterResult scatterResult = rayCastResult.Material.Scatter(rayCastResult.HitInfo, rayDesc.Direction);

				emissiveColor += attenuation * rayCastResult.Material.EmissiveColor.rgb;
				attenuation *= scatterResult.Attenuation;

				if (depth == 1) traceResult.HitDistance = rayCastResult.HitDistance;
				else if (g_graphicsSettings.IsRussianRouletteEnabled && depth > 3) {
					const float probability = max(attenuation.r, max(attenuation.g, attenuation.b));
					if (STL::Rng::Hash::GetFloat() >= probability) break;
					attenuation /= probability;
				}

				rayDesc.Origin = rayCastResult.HitInfo.Vertex.Position;
				rayDesc.Direction = scatterResult.Direction;
			}
			else {
				incidentColor = GetEnvironmentLightColor(rayDesc.Direction);
				break;
			}
		}

		traceResult.Radiance = emissiveColor + incidentColor * attenuation;

		return traceResult;
	}
};
