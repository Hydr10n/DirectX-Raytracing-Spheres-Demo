#pragma once

#include "Math.hlsli"

#include "HitInfo.hlsli"

#include "ShadingHelpers.hlsli"

template <uint Flags>
float3 TraceRay(
	inout RayQuery<Flags> q, RayDesc rayDesc, uint flags, uint mask
#ifdef NV_SHADER_EXTN_SLOT
	, bool enableShaderExecutionReordering = false
#endif
)
{
	float3 visibility = 1;
	q.TraceRayInline(g_scene, flags, mask, rayDesc);
	while (q.Proceed())
	{
		if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
		{
			const ObjectData objectData = g_objectData[q.CandidateInstanceID() + q.CandidateGeometryIndex()];
			const float2 textureCoordinate = GetTextureCoordinate(objectData, q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics());
			if (Flags & RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH)
			{
				if (IsOpaque(objectData, textureCoordinate, visibility))
				{
					q.CommitNonOpaqueTriangleHit();
				}
			}
			else if (IsOpaque(objectData, textureCoordinate))
			{
				q.CommitNonOpaqueTriangleHit();
			}
		}
	}
#ifdef NV_SHADER_EXTN_SLOT
	if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT && enableShaderExecutionReordering)
	{
		const BuiltInTriangleIntersectionAttributes attributes = { q.CommittedTriangleBarycentrics() };
		const NvHitObject hitObject = NvMakeHit(g_scene, q.CommittedInstanceIndex(), q.CommittedGeometryIndex(), q.CommittedPrimitiveIndex(), 0, 0, 1, rayDesc, attributes);
		NvReorderThread(hitObject);
	}
#endif
	return visibility;
}

bool CastRay(
	RayDesc rayDesc, out HitInfo hitInfo
#ifdef NV_SHADER_EXTN_SLOT
	, bool enableShaderExecutionReordering = false
#endif
)
{
	RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
	TraceRay(
		q, rayDesc, RAY_FLAG_NONE, ~0u
#ifdef NV_SHADER_EXTN_SLOT
		, enableShaderExecutionReordering
#endif
	);
	hitInfo.Distance = 1.#INF;
	const bool isHit = q.CommittedStatus() != COMMITTED_NOTHING;
	if (isHit)
	{
		hitInfo.InstanceIndex = q.CommittedInstanceIndex();
		hitInfo.ObjectIndex = q.CommittedInstanceID() + q.CommittedGeometryIndex();
		hitInfo.PrimitiveIndex = q.CommittedPrimitiveIndex();

		const ObjectData objectData = g_objectData[hitInfo.ObjectIndex];
		const MeshResourceDescriptorIndices meshIndices = objectData.ResourceDescriptorIndices.Mesh;
		const TextureMapResourceDescriptorIndices textureMapIndices = objectData.ResourceDescriptorIndices.TextureMaps;
		const uint3 indices = MeshHelpers::Load3Indices(ResourceDescriptorHeap[meshIndices.Indices], hitInfo.PrimitiveIndex);
		const ByteAddressBuffer vertices = ResourceDescriptorHeap[meshIndices.Vertices];
		const VertexDesc vertexDesc = objectData.VertexDesc;
		float3 positions[3], normals[3];
		float2 textureCoordinates[3];
		vertexDesc.LoadPositions(vertices, indices, positions);
		vertexDesc.LoadNormals(vertices, indices, normals);
		vertexDesc.LoadTextureCoordinates(vertices, indices, textureCoordinates);
		hitInfo.Initialize(
			positions, normals, textureCoordinates,
			q.CommittedTriangleBarycentrics(),
			q.CommittedObjectToWorld3x4(), q.CommittedWorldToObject3x4(),
			rayDesc.Direction, q.CommittedRayT()
		);
		if (textureMapIndices.Normal != ~0u)
		{
			const Texture2D<float3> texture = ResourceDescriptorHeap[textureMapIndices.Normal];
			float3 T;
			if (vertexDesc.TangentOffset == ~0u)
			{
				T = normalize(Math::CalculateTangent(positions, textureCoordinates));
			}
			else
			{
				float3 tangents[3];
				vertexDesc.LoadTangents(vertices, indices, tangents);
				T = normalize(Vertex::Interpolate(tangents, hitInfo.Barycentrics));
			}
			const float3x3 TBN = float3x3(T, normalize(cross(hitInfo.Normal, T)), hitInfo.Normal);
			hitInfo.Normal = normalize(Geometry::RotateVectorInverse(TBN, texture.SampleLevel(g_anisotropicSampler, hitInfo.TextureCoordinate, 0) * 2 - 1));
		}
	}
	return isHit;
}
