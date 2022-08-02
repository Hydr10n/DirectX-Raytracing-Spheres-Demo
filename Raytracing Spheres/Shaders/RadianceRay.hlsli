#pragma once

#include "Common.hlsli"

float4 TraceRadianceRay(RayDesc rayDesc, inout Random random) {
	if (!g_globalData.RaytracingMaxTraceRecursionDepth) return 0;

	float4 accumulatedColor = 1;

	RayQuery<RAY_FLAG_NONE> q;
	for (uint depth = 0; depth < g_globalData.RaytracingMaxTraceRecursionDepth; depth++) {
		q.TraceRayInline(g_scene, RAY_FLAG_NONE, ~0, rayDesc);

		q.Proceed();

		switch (q.CommittedStatus()) {
		case COMMITTED_TRIANGLE_HIT: {
			const uint instanceID = q.CommittedInstanceID();

			const LocalResourceDescriptorHeapIndices localResourceDescriptorHeapIndices = g_localResourceDescriptorHeapIndices[instanceID];
			const LocalData localData = g_localData[instanceID];

			const float3 worldRayDirection = q.WorldRayDirection();

			const HitInfo hitInfo = GetHitInfo(instanceID, q.WorldRayOrigin(), worldRayDirection, q.CommittedRayT(), q.CommittedObjectToWorld4x3(), q.CommittedPrimitiveIndex(), q.CommittedTriangleBarycentrics());

			float4 color;
			if (localResourceDescriptorHeapIndices.Textures.ColorMap != UINT_MAX) {
				const Texture2D<float4> colorMap = ResourceDescriptorHeap[localResourceDescriptorHeapIndices.Textures.ColorMap];
				color = colorMap.SampleLevel(g_anisotropicWrap, hitInfo.Vertex.TextureCoordinate, 0);
			}
			else color = localData.Material.Color;

			if (localData.Material.Scatter(worldRayDirection, hitInfo, rayDesc.Direction, random)) {
				accumulatedColor *= color;

				rayDesc.Origin = hitInfo.Vertex.Position;
			}
			else return accumulatedColor * (localData.Material.IsEmissive() ? color : 0);
		} break;

		case COMMITTED_NOTHING: {
			float4 color;
			if (g_globalResourceDescriptorHeapIndices.EnvironmentCubeMap != UINT_MAX) {
				color = g_environmentCubeMap.SampleLevel(g_anisotropicWrap, mul(q.WorldRayDirection(), (float3x3) g_globalData.EnvironmentMapTransform), 0);
			}
			else color = lerp(float4(1, 1, 1, 1), float4(0.5f, 0.7f, 1, 1), 0.5f * normalize(q.WorldRayDirection()).y + 0.5f);

			return accumulatedColor * color;
		}
		}
	}

	return accumulatedColor;
}
