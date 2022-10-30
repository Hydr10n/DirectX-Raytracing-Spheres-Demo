#pragma once

#include "Common.hlsli"

#include "Stack.hlsli"

struct RadianceRay {
	static float4 Trace(RayDesc rayDesc, inout Random random) {
		float4 reflectedColor;

		struct ScatterInfo { float4 EmissiveColor, Attenuation; };
		Stack<ScatterInfo, RaytracingMaxTraceRecursionDepth> scatterInfos;
		scatterInfos.Initialize();

		RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
		for (uint depth = 0, maxDepth = min(RaytracingMaxTraceRecursionDepth, g_globalData.RaytracingMaxTraceRecursionDepth); ; depth++) {
			if (depth == maxDepth) {
				reflectedColor = depth == 1;

				break;
			}

			q.TraceRayInline(g_scene, RAY_FLAG_NONE, ~0u, rayDesc);
			q.Proceed();

			const float3 worldRayDirection = q.WorldRayDirection();

			if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
				const uint instanceIndex = q.CommittedInstanceIndex();

				const HitInfo hitInfo = GetHitInfo(instanceIndex, q.WorldRayOrigin(), worldRayDirection, q.CommittedRayT(), q.CommittedObjectToWorld4x3(), q.CommittedPrimitiveIndex(), q.CommittedTriangleBarycentrics());

				const Material material = GetMaterial(instanceIndex, hitInfo.Vertex.TextureCoordinate);

				ScatterInfo scatterInfo;

				scatterInfo.EmissiveColor = material.EmissiveColor;

				material.Scatter(hitInfo, worldRayDirection, rayDesc.Direction, scatterInfo.Attenuation, random);

				scatterInfos.Push(scatterInfo);

				rayDesc.Origin = hitInfo.Vertex.Position;
			}
			else {
				if (!depth) {
					if (g_globalResourceDescriptorHeapIndices.EnvironmentCubeMap != ~0u) {
						return g_environmentCubeMap.SampleLevel(g_anisotropicWrap, normalize(mul(worldRayDirection, (float3x3)g_globalData.EnvironmentCubeMapTransform)), 0);
					}
					if (g_globalData.EnvironmentColor.a >= 0) return g_globalData.EnvironmentColor;
				}

				if (g_globalResourceDescriptorHeapIndices.EnvironmentLightCubeMap != ~0u) {
					reflectedColor = g_environmentLightCubeMap.SampleLevel(g_anisotropicWrap, normalize(mul(worldRayDirection, (float3x3)g_globalData.EnvironmentLightCubeMapTransform)), 0);
				}
				else if (g_globalData.EnvironmentLightColor.a >= 0) reflectedColor = g_globalData.EnvironmentLightColor;
				else reflectedColor = lerp(float4(1, 1, 1, 1), float4(0.5f, 0.7f, 1, 1), 0.5f * worldRayDirection.y + 0.5f);

				break;
			}
		}

		while (!scatterInfos.IsEmpty()) {
			const ScatterInfo scatterInfo = scatterInfos.GetTop();
			reflectedColor = scatterInfo.EmissiveColor + scatterInfo.Attenuation * reflectedColor;
			scatterInfos.Pop();
		}

		return reflectedColor;
	}
};
