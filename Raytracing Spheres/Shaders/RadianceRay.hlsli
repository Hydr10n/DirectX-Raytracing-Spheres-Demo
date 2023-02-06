#pragma once

#include "Raytracing.hlsli"

struct RadianceRay {
	static float4 Trace(RayDesc rayDesc, inout Random random) {
		float4 emissiveColor = 0, attenuation = 1, environmentColor;

		RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
		for (uint depth = 0; ; depth++) {
			if (depth == g_globalData.RaytracingMaxTraceRecursionDepth) {
				environmentColor = depth == 1;

				break;
			}

			q.TraceRayInline(g_scene, RAY_FLAG_NONE, ~0u, rayDesc);
			q.Proceed();

			if (q.CommittedStatus() == COMMITTED_NOTHING) {
				if (!depth) {
					if (g_globalResourceDescriptorHeapIndices.EnvironmentCubeMap != ~0u) {
						return g_environmentCubeMap.SampleLevel(g_anisotropicSampler, normalize(mul((float3x3)g_globalData.EnvironmentCubeMapTransform, rayDesc.Direction)), 0);
					}
					if (g_globalData.EnvironmentColor.a >= 0) return g_globalData.EnvironmentColor;
				}

				if (g_globalResourceDescriptorHeapIndices.EnvironmentLightCubeMap != ~0u) {
					environmentColor = g_environmentLightCubeMap.SampleLevel(g_anisotropicSampler, normalize(mul((float3x3)g_globalData.EnvironmentLightCubeMapTransform, rayDesc.Direction)), 0);
				}
				else if (g_globalData.EnvironmentLightColor.a >= 0) environmentColor = g_globalData.EnvironmentLightColor;
				else environmentColor = lerp(float4(1, 1, 1, 1), float4(0.5f, 0.7f, 1, 1), 0.5f * rayDesc.Direction.y + 0.5f);

				break;
			}

			const uint instanceIndex = q.CommittedInstanceIndex();

			const HitInfo hitInfo = GetHitInfo(instanceIndex, q.WorldRayOrigin(), rayDesc.Direction, q.CommittedRayT(), q.CommittedObjectToWorld3x4(), q.CommittedPrimitiveIndex(), q.CommittedTriangleBarycentrics());

			const Material material = GetMaterial(instanceIndex, hitInfo.Vertex.TextureCoordinate);

			const ScatterResult scatterResult = material.Scatter(hitInfo, rayDesc.Direction, random);

			emissiveColor += attenuation * material.EmissiveColor;
			attenuation *= scatterResult.Attenuation;

			rayDesc.Origin = hitInfo.Vertex.Position;
			rayDesc.Direction = scatterResult.Direction;
			rayDesc.TMin = 1e-3f;
			rayDesc.TMax = 1.#INFf;
		}

		return emissiveColor + attenuation * environmentColor;
	}
};
