#include "Common.hlsli"

#include "TriangleLight.hlsli"

#include "MeshHelpers.hlsli"

SamplerState g_anisotropicSampler : register(s0);

cbuffer _ : register(b0) { uint g_taskCount; }

struct Task { uint InstanceIndex, GeometryIndex, TriangleCount, LightBufferOffset; };
StructuredBuffer<Task> g_tasks : register(t0);

StructuredBuffer<InstanceData> g_instanceData : register(t1);
StructuredBuffer<ObjectData> g_objectData : register(t2);

RWStructuredBuffer<RAB_LightInfo> g_lightInfo : register(u0);

bool FindTask(uint dispatchThreadID, out Task task) {
	for (int left = 0, right = int(g_taskCount) - 1; left <= right;) {
		const int middle = left + (right - left) / 2;
		task = g_tasks[middle];
		const int triangleIndex = int(dispatchThreadID) - int(task.LightBufferOffset);
		if (triangleIndex < 0) right = middle - 1;
		else if (triangleIndex < task.TriangleCount) return true;
		else left = middle + 1;
	}
	return false;
}

[RootSignature(
	"RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED),"
	"StaticSampler(s0),"
	"RootConstants(num32BitConstants=1, b0),"
	"SRV(t0),"
	"SRV(t1),"
	"SRV(t2),"
	"UAV(u0)"
)]
[numthreads(256, 1, 1)]
void main(uint dispatchThreadID : SV_DispatchThreadID) {
	Task task;
	if (!FindTask(dispatchThreadID, task)) return;

	const InstanceData instanceData = g_instanceData[task.InstanceIndex];
	const uint objectIndex = instanceData.FirstGeometryIndex + task.GeometryIndex;
	const ObjectResourceDescriptorIndices resourceDescriptorIndices = g_objectData[objectIndex].ResourceDescriptorIndices;
	const uint3 indices = MeshHelpers::Load3Indices(ResourceDescriptorHeap[resourceDescriptorIndices.Mesh.Indices], dispatchThreadID - task.LightBufferOffset);
	const ByteAddressBuffer vertices = ResourceDescriptorHeap[resourceDescriptorIndices.Mesh.Vertices];
	const VertexDesc vertexDesc = g_objectData[objectIndex].VertexDesc;

	float3 positions[3];
	vertexDesc.LoadPositions(vertices, indices, positions);
	positions[0] = STL::Geometry::AffineTransform(instanceData.ObjectToWorld, positions[0]);
	positions[1] = STL::Geometry::AffineTransform(instanceData.ObjectToWorld, positions[1]);
	positions[2] = STL::Geometry::AffineTransform(instanceData.ObjectToWorld, positions[2]);

	float3 emissiveColor;
	if (resourceDescriptorIndices.TextureMaps.EmissiveColor == ~0u) emissiveColor = g_objectData[objectIndex].Material.EmissiveColor;
	else {
		float2 textureCoordinates[3];
		vertexDesc.LoadTextureCoordinates(vertices, indices, textureCoordinates);

		const float2 edges[] = { textureCoordinates[1] - textureCoordinates[0], textureCoordinates[2] - textureCoordinates[1], textureCoordinates[0] - textureCoordinates[2] };
		const float edgeLengths[] = { length(edges[0]), length(edges[1]), length(edges[2]) };

		float2 shortEdge, longEdges[2];
		if (edgeLengths[0] < edgeLengths[1] && edgeLengths[0] < edgeLengths[2]) {
			shortEdge = edges[0];
			longEdges[0] = edges[1];
			longEdges[1] = edges[2];
		}
		else if (edgeLengths[1] < edgeLengths[2]) {
			shortEdge = edges[1];
			longEdges[0] = edges[2];
			longEdges[1] = edges[0];
		}
		else {
			shortEdge = edges[2];
			longEdges[0] = edges[0];
			longEdges[1] = edges[1];
		}

		const Texture2D<float3> texture = ResourceDescriptorHeap[resourceDescriptorIndices.TextureMaps.EmissiveColor];
		emissiveColor = texture.SampleGrad(g_anisotropicSampler, (textureCoordinates[0] + textureCoordinates[1] + textureCoordinates[2]) / 3, shortEdge * (2.0f / 3), (longEdges[0] + longEdges[1]) / 3).rgb;
	}

	RAB_LightInfo lightInfo;
	lightInfo.Base = positions[0];
	lightInfo.Edges[0] = positions[1] - positions[0];
	lightInfo.Edges[1] = positions[2] - positions[0];
	lightInfo.Radiance = emissiveColor;
	g_lightInfo[dispatchThreadID] = lightInfo;
}
