#pragma once

#include "Common.hlsli"

struct RadianceRay {
	static float4 Trace(RayDesc rayDesc, inout Random random) {
		if (!g_globalData.RaytracingMaxTraceRecursionDepth) return 0;

		float4 reflectedColor = 1;

		RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
		for (uint depth = 0; depth < g_globalData.RaytracingMaxTraceRecursionDepth; depth++) {
			q.TraceRayInline(g_scene, RAY_FLAG_NONE, ~0, rayDesc);
			q.Proceed();

			const float3 worldRayDirection = q.WorldRayDirection();

			if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
				const uint instanceID = q.CommittedInstanceID();

				const HitInfo hitInfo = GetHitInfo(instanceID, q.WorldRayOrigin(), worldRayDirection, q.CommittedRayT(), q.CommittedObjectToWorld4x3(), q.CommittedPrimitiveIndex(), q.CommittedTriangleBarycentrics());

				const Material material = GetMaterial(instanceID, hitInfo.Vertex.TextureCoordinate);

				float4 attenuation;
				if (material.Scatter(worldRayDirection, hitInfo, rayDesc.Direction, attenuation, random)) {
					reflectedColor *= attenuation;

					rayDesc.Origin = hitInfo.Vertex.Position;
				}
				else return reflectedColor * (material.IsEmissive() ? attenuation : 0);
			}
			else {
				float4 color;
				if (g_globalResourceDescriptorHeapIndices.EnvironmentCubeMap != ~0u) {
					color = g_environmentCubeMap.SampleLevel(g_anisotropicWrap, mul(worldRayDirection, (float3x3)g_globalData.EnvironmentMapTransform), 0);
				}
				else color = lerp(float4(1, 1, 1, 1), float4(0.5f, 0.7f, 1, 1), 0.5f * worldRayDirection.y + 0.5f);

				return reflectedColor * color;
			}
		}

		return reflectedColor;
	}
};
