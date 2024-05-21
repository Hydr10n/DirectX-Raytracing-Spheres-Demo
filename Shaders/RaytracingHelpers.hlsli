#pragma once

#include "Math.hlsli"

#include "ShadingHelpers.hlsli"

template <uint Flags>
void TraceRay(
	inout RayQuery<Flags> q, RayDesc rayDesc, uint flags, uint mask
#ifdef NV_SHADER_EXTN_SLOT
	, bool enableShaderExecutionReordering
#endif
)
{
	q.TraceRayInline(g_scene, flags, mask, rayDesc);
	while (q.Proceed())
	{
		if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
		{
			const uint objectIndex = g_instanceData[q.CandidateInstanceIndex()].FirstGeometryIndex + q.CandidateGeometryIndex();
			const float2 textureCoordinate = GetTextureCoordinate(objectIndex, q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics());
			if (IsOpaque(objectIndex, textureCoordinate))
			{
				q.CommitNonOpaqueTriangleHit();
			}
		}
	}
#ifdef NV_SHADER_EXTN_SLOT
	if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT && enableShaderExecutionReordering)
	{
		const BuiltInTriangleIntersectionAttributes attributes = { q.CommittedTriangleBarycentrics() };
		const NvHitObject hitObject = NvMakeHit(g_scene, q.CommittedInstanceIndex(), q.CommittedGeometryIndex(), q.CommittedPrimitiveIndex(), 0, 0, 0, rayDesc, attributes);
		NvReorderThread(hitObject);
	}
#endif
}

bool CastRay(
	RayDesc rayDesc, out HitInfo hitInfo
#ifdef NV_SHADER_EXTN_SLOT
	, bool enableShaderExecutionReordering
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
	const bool isHit = q.CommittedStatus() != COMMITTED_NOTHING;
	if (isHit)
	{
		hitInfo.InstanceIndex = q.CommittedInstanceIndex();
		hitInfo.ObjectIndex = g_instanceData[hitInfo.InstanceIndex].FirstGeometryIndex + q.CommittedGeometryIndex();
		hitInfo.PrimitiveIndex = q.CommittedPrimitiveIndex();

		const ObjectResourceDescriptorIndices resourceDescriptorIndices = g_objectData[hitInfo.ObjectIndex].ResourceDescriptorIndices;
		const uint3 indices = MeshHelpers::Load3Indices(ResourceDescriptorHeap[resourceDescriptorIndices.Mesh.Indices], hitInfo.PrimitiveIndex);
		const ByteAddressBuffer vertices = ResourceDescriptorHeap[resourceDescriptorIndices.Mesh.Vertices];
		const VertexDesc vertexDesc = g_objectData[hitInfo.ObjectIndex].VertexDesc;
		float3 positions[3], normals[3];
		float2 textureCoordinates[3];
		vertexDesc.LoadPositions(vertices, indices, positions);
		vertexDesc.LoadNormals(vertices, indices, normals);
		vertexDesc.LoadTextureCoordinates(vertices, indices, textureCoordinates);
		hitInfo.Initialize(positions, normals, textureCoordinates, q.CommittedTriangleBarycentrics(), q.CommittedObjectToWorld3x4(), q.CommittedWorldToObject3x4(), rayDesc.Origin, rayDesc.Direction, q.CommittedRayT());
		if (resourceDescriptorIndices.TextureMaps.Normal != ~0u)
		{
			const Texture2D<float3> texture = ResourceDescriptorHeap[resourceDescriptorIndices.TextureMaps.Normal];
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
			hitInfo.Normal = normalize(STL::Geometry::RotateVectorInverse(TBN, texture.SampleLevel(g_anisotropicSampler, hitInfo.TextureCoordinate, 0) * 2 - 1));
		}
	}
	return isHit;
}
