#include "Common.hlsli"

#include "TriangleLight.hlsli"

#include "MeshHelpers.hlsli"

SamplerState g_anisotropicSampler : register(s0);

struct Constants {
	bool IgnoreStatic;
	uint TaskCount;
};
ConstantBuffer<Constants> g_constants : register(b0);

struct Task { uint InstanceIndex, GeometryIndex, TriangleCount, LightBufferOffset; };
StructuredBuffer<Task> g_tasks : register(t0);

StructuredBuffer<InstanceData> g_instanceData : register(t1);
StructuredBuffer<ObjectData> g_objectData : register(t2);

RWStructuredBuffer<RAB_LightInfo> g_lightInfo : register(u0);

bool FindTask(uint dispatchThreadID, out Task task) {
	for (int left = 0, right = int(g_constants.TaskCount) - 1; left <= right;) {
		const int middle = (left + right) / 2;
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
	"RootConstants(num32BitConstants=2, b0),"
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

	if (g_constants.IgnoreStatic && all(instanceData.PreviousObjectToWorld == instanceData.ObjectToWorld)) return;

	const uint objectIndex = g_instanceData[task.InstanceIndex].FirstGeometryIndex + task.GeometryIndex;

	const ObjectResourceDescriptorHeapIndices resourceDescriptorHeapIndices = g_objectData[objectIndex].ResourceDescriptorHeapIndices;

	const StructuredBuffer<VertexPositionNormalTexture> vertices = ResourceDescriptorHeap[resourceDescriptorHeapIndices.Mesh.Vertices];
	const uint3 indices = MeshHelpers::Load3Indices(ResourceDescriptorHeap[resourceDescriptorHeapIndices.Mesh.Indices], dispatchThreadID - task.LightBufferOffset);
	const float3 positions[] = {
		STL::Geometry::AffineTransform(instanceData.ObjectToWorld, vertices[indices[0]].Position),
		STL::Geometry::AffineTransform(instanceData.ObjectToWorld, vertices[indices[1]].Position),
		STL::Geometry::AffineTransform(instanceData.ObjectToWorld, vertices[indices[2]].Position)
	};

	float3 emissiveColor;
	if (resourceDescriptorHeapIndices.Textures.EmissiveColorMap == ~0u) emissiveColor = g_objectData[objectIndex].Material.EmissiveColor;
	else {
		const float2 textureCoordinates[] = { vertices[indices[0]].TextureCoordinate, vertices[indices[1]].TextureCoordinate, vertices[indices[2]].TextureCoordinate };
		const float2 edges[3] = { textureCoordinates[1] - textureCoordinates[0], textureCoordinates[2] - textureCoordinates[1], textureCoordinates[0] - textureCoordinates[2] };
		const float3 edgeLengths = float3(length(edges[0]), length(edges[1]), length(edges[2]));

		float2 shortEdge;
		float2 longEdge1;
		float2 longEdge2;
		if (edgeLengths[0] < edgeLengths[1] && edgeLengths[0] < edgeLengths[2]) {
			shortEdge = edges[0];
			longEdge1 = edges[1];
			longEdge2 = edges[2];
		}
		else if (edgeLengths[1] < edgeLengths[2]) {
			shortEdge = edges[1];
			longEdge1 = edges[2];
			longEdge2 = edges[0];
		}
		else {
			shortEdge = edges[2];
			longEdge1 = edges[0];
			longEdge2 = edges[1];
		}

		const Texture2D<float3> texture = ResourceDescriptorHeap[resourceDescriptorHeapIndices.Textures.EmissiveColorMap];
		emissiveColor = texture.SampleGrad(g_anisotropicSampler, (textureCoordinates[0] + textureCoordinates[1] + textureCoordinates[2]) / 3, shortEdge * (2.0f / 3), (longEdge1 + longEdge2) / 3).rgb;
	}

	RAB_LightInfo lightInfo;
	lightInfo.Base = positions[0];
	lightInfo.Edge1 = positions[1] - positions[0];
	lightInfo.Edge2 = positions[2] - positions[0];
	lightInfo.Radiance = emissiveColor;
	g_lightInfo[dispatchThreadID] = lightInfo;
}
