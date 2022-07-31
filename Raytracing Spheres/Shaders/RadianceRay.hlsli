#pragma once

#include "Common.hlsli"

#include "TriangleMeshIndexHelpers.hlsli"

#include "MathHelpers.hlsli"

inline float4 TraceRadianceRay(RayDesc rayDesc, inout Random random) {
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

			float3 worldNormal;
			float2 textureCoordinate;
			{
				const StructuredBuffer<VertexPositionNormalTexture> vertices = ResourceDescriptorHeap[localResourceDescriptorHeapIndices.TriangleMesh.Vertices];
				const uint3 indices = TriangleMeshIndexHelpers::Load3Indices(ResourceDescriptorHeap[localResourceDescriptorHeapIndices.TriangleMesh.Indices], q.CommittedPrimitiveIndex());
				const float3 normals[] = { vertices[indices[0]].Normal, vertices[indices[1]].Normal, vertices[indices[2]].Normal };
				const float2 textureCoordinates[] = { vertices[indices[0]].TextureCoordinate, vertices[indices[1]].TextureCoordinate, vertices[indices[2]].TextureCoordinate };

				const float2 barycentrics = q.CommittedTriangleBarycentrics();

				const float3 normal = Vertex::Interpolate(normals, barycentrics);
				worldNormal = normalize(mul(normal, (float3x3) q.CommittedObjectToWorld4x3()));

				textureCoordinate = Vertex::Interpolate(textureCoordinates, barycentrics);
				textureCoordinate = mul(float4(textureCoordinate, 0, 1), localData.TextureTransform).xy;

				if (localResourceDescriptorHeapIndices.Textures.NormalMap != UINT_MAX) {
					const Texture2D<float3> normalMap = ResourceDescriptorHeap[localResourceDescriptorHeapIndices.Textures.NormalMap];
					const float3x3 TBN = MathHelpers::CalculateTBN(worldNormal, worldRayDirection);
					worldNormal = normalize(mul(normalize(normalMap.SampleLevel(g_anisotropicWrap, textureCoordinate, 0) * 2 - 1), TBN));
				}
			}

			HitInfo hitInfo;
			hitInfo.Vertex.Position = q.WorldRayOrigin() + worldRayDirection * q.CommittedRayT();
			hitInfo.SetFaceNormal(worldNormal, worldRayDirection);

			float4 color;
			if (localResourceDescriptorHeapIndices.Textures.ColorMap != UINT_MAX) {
				const Texture2D<float4> colorMap = ResourceDescriptorHeap[localResourceDescriptorHeapIndices.Textures.ColorMap];
				color = colorMap.SampleLevel(g_anisotropicWrap, textureCoordinate, 0);
			}
			else color = localData.Material.Color;

			float3 direction = 0;
			if (localData.Material.Scatter(worldRayDirection, hitInfo, direction, random)) {
				accumulatedColor *= color;

				rayDesc.Origin = hitInfo.Vertex.Position;
				rayDesc.Direction = direction;
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
