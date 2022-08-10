#pragma once

#include "Common.hlsli"

struct RadianceRay {
	static float4 Trace(RayDesc rayDesc, inout Random random) {
		if (!g_globalData.RaytracingMaxTraceRecursionDepth) return 0;

		float4 accumulatedColor = 1;

		RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
		for (uint depth = 0; depth < g_globalData.RaytracingMaxTraceRecursionDepth; depth++) {
			q.TraceRayInline(g_scene, RAY_FLAG_NONE, ~0, rayDesc);

			q.Proceed();

			if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
				const uint instanceID = q.CommittedInstanceID();

				const float3 worldRayDirection = q.WorldRayDirection();

				const HitInfo hitInfo = GetHitInfo(instanceID, q.WorldRayOrigin(), worldRayDirection, q.CommittedRayT(), q.CommittedObjectToWorld4x3(), q.CommittedPrimitiveIndex(), q.CommittedTriangleBarycentrics());

				const Material material = g_localData[instanceID].Material;

				float4 color;
				const uint colorMapIndex = g_localResourceDescriptorHeapIndices[instanceID].Textures.ColorMap;
				if (colorMapIndex != UINT_MAX) {
					const Texture2D<float4> colorMap = ResourceDescriptorHeap[colorMapIndex];
					color = colorMap.SampleLevel(g_anisotropicWrap, hitInfo.Vertex.TextureCoordinate, 0);
				}
				else color = material.Color;

				if (material.Scatter(worldRayDirection, hitInfo, rayDesc.Direction, random)) {
					accumulatedColor *= color;

					rayDesc.Origin = hitInfo.Vertex.Position;
				}
				else return accumulatedColor * (material.IsEmissive() ? color : 0);
			}
			else {
				float4 color;
				if (g_globalResourceDescriptorHeapIndices.EnvironmentCubeMap != UINT_MAX) {
					color = g_environmentCubeMap.SampleLevel(g_anisotropicWrap, mul(q.WorldRayDirection(), (float3x3) g_globalData.EnvironmentMapTransform), 0);
				}
				else color = lerp(float4(1, 1, 1, 1), float4(0.5f, 0.7f, 1, 1), 0.5f * normalize(q.WorldRayDirection()).y + 0.5f);

				return accumulatedColor * color;
			}
		}

		return accumulatedColor;
	}
};
