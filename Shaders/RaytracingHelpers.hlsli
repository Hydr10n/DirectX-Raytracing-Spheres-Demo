#pragma once

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
			const MeshDescriptors meshDescriptors = objectData.MeshDescriptors;
			const ByteAddressBuffer vertices = ResourceDescriptorHeap[meshDescriptors.Vertices];
			const uint3 indices = MeshHelpers::Load3Indices(ResourceDescriptorHeap[meshDescriptors.Indices], q.CandidatePrimitiveIndex());
			float2 textureCoordinates[2];
			GetTextureCoordinates(
				objectData.VertexDesc,
				vertices,
				indices,
				q.CandidateTriangleBarycentrics(),
				textureCoordinates
			);
			if (Flags & RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH)
			{
				if (IsOpaque(objectData.Material, objectData.TextureMapInfoArray, textureCoordinates, visibility))
				{
					q.CommitNonOpaqueTriangleHit();
				}
			}
			else if (IsOpaque(objectData.Material, objectData.TextureMapInfoArray, textureCoordinates))
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
	hitInfo.Position = rayDesc.Origin + rayDesc.Direction * 1e8f;
	hitInfo.Distance = 1.#INF;

	RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
	TraceRay(
		q, rayDesc, RAY_FLAG_NONE, ~0u
#ifdef NV_SHADER_EXTN_SLOT
		, enableShaderExecutionReordering
#endif
	);
	const bool isHit = q.CommittedStatus() != COMMITTED_NOTHING;
	if (isHit)
	{
		hitInfo.Distance = q.CommittedRayT();
		hitInfo.InstanceIndex = q.CommittedInstanceIndex();
		hitInfo.ObjectIndex = q.CommittedInstanceID() + q.CommittedGeometryIndex();
		hitInfo.PrimitiveIndex = q.CommittedPrimitiveIndex();

		const float3x4 objectToWorld = q.CommittedObjectToWorld3x4();
		const ObjectData objectData = g_objectData[hitInfo.ObjectIndex];

		const MeshDescriptors meshDescriptors = objectData.MeshDescriptors;
		const ByteAddressBuffer vertices = ResourceDescriptorHeap[meshDescriptors.Vertices];
		const uint3 indices = MeshHelpers::Load3Indices(ResourceDescriptorHeap[meshDescriptors.Indices], hitInfo.PrimitiveIndex);
		const VertexDesc vertexDesc = objectData.VertexDesc;

		float3 positions[3];
		vertexDesc.LoadPositions(vertices, indices, positions);

		if (vertexDesc.AttributeOffsets.Normal != ~0u)
		{
			float3 normals[3];
			vertexDesc.LoadNormals(vertices, indices, normals);

			hitInfo.Initialize(
				positions, normals,
				q.CommittedTriangleBarycentrics(),
				objectToWorld, q.CommittedWorldToObject3x4(),
				rayDesc.Direction
			);
		}
		else
		{
			hitInfo.Initialize(
				positions,
				q.CommittedTriangleBarycentrics(),
				objectToWorld, q.CommittedWorldToObject3x4(),
				rayDesc.Direction
			);
		}

		hitInfo.Tangent = 0;
		if (vertexDesc.AttributeOffsets.Tangent != ~0u)
		{
			float3 tangents[3];
			vertexDesc.LoadTangents(vertices, indices, tangents);
			hitInfo.Tangent = Vertex::Interpolate(tangents, hitInfo.Barycentrics);
			hitInfo.Tangent = normalize(Geometry::RotateVector((float3x3)objectToWorld, hitInfo.Tangent));
		}

		GetTextureCoordinates(
			objectData.VertexDesc,
			vertices,
			indices,
			q.CommittedTriangleBarycentrics(),
			hitInfo.TextureCoordinates
		);
	}
	return isHit;
}
