#pragma once

#include "Common.hlsli"

#include "Stack.hlsli"

struct RadianceRay {
	static float4 Trace(RayDesc rayDesc, inout Random random) {
		float4 reflectedColor;

		struct ScatterInfo { float4 EmissiveColor, Attenuation; };
		Stack<ScatterInfo, RaytracingMaxDeclarableTraceRecursionDepth> scatterInfos;
		scatterInfos.Initialize();

		RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
		for (uint depth = 0, maxDepth = min(RaytracingMaxDeclarableTraceRecursionDepth, g_globalData.RaytracingMaxTraceRecursionDepth); ; depth++) {
			if (depth == maxDepth) {
				reflectedColor = depth == 1;

				break;
			}

			q.TraceRayInline(g_scene, RAY_FLAG_NONE, ~0u, rayDesc);
			q.Proceed();

			const float3 worldRayDirection = q.WorldRayDirection();

			if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
				const uint instanceID = q.CommittedInstanceID();

				const HitInfo hitInfo = GetHitInfo(instanceID, q.WorldRayOrigin(), worldRayDirection, q.CommittedRayT(), q.CommittedObjectToWorld4x3(), q.CommittedPrimitiveIndex(), q.CommittedTriangleBarycentrics());

				const Material material = GetMaterial(instanceID, hitInfo.Vertex.TextureCoordinate);

				ScatterInfo scatterInfo;

				scatterInfo.EmissiveColor = material.EmissiveColor;

				material.Scatter(worldRayDirection, hitInfo, rayDesc.Direction, scatterInfo.Attenuation, random);

				scatterInfos.Push(scatterInfo);

				rayDesc.Origin = hitInfo.Vertex.Position;
			}
			else {
				if (g_globalResourceDescriptorHeapIndices.EnvironmentCubeMap != ~0u) {
					reflectedColor = g_environmentCubeMap.SampleLevel(g_anisotropicWrap, mul(worldRayDirection, (float3x3)g_globalData.EnvironmentMapTransform), 0);
				}
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
