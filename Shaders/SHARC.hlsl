#include "SharcCommon.h"

#include "Camera.hlsli"

struct Constants
{
	bool IsResolve;
	uint Capacity, AccumulationFrames, MaxStaleFrames;
	float SceneScale;
};
ConstantBuffer<Constants> g_constants : register(b0);

ConstantBuffer<Camera> g_camera : register(b1);

RWStructuredBuffer<uint64_t> g_hashEntries : register(u0);
RWStructuredBuffer<uint> g_hashCopyOffset : register(u1);
RWStructuredBuffer<uint4> g_previousVoxelData : register(u2);
RWStructuredBuffer<uint4> g_voxelData : register(u3);

[RootSignature(
	"RootConstants(num32BitConstants=5, b0),"
	"CBV(b1),"
	"UAV(u0),"
	"UAV(u1),"
	"UAV(u2),"
	"UAV(u3)"
)]
[numthreads(256, 1, 1)]
void main(uint dispatchThreadID : SV_DispatchThreadID)
{
	HashMapData hashMapData;
	hashMapData.capacity = g_constants.Capacity;
	hashMapData.hashEntriesBuffer = g_hashEntries;
	if (g_constants.IsResolve)
	{
		GridParameters gridParameters;
		gridParameters.cameraPosition = g_camera.Position;
		gridParameters.cameraPositionPrev = g_camera.PreviousPosition;
		gridParameters.sceneScale = g_constants.SceneScale;
		gridParameters.logarithmBase = SHARC_GRID_LOGARITHM_BASE;
		SharcResolveEntry(dispatchThreadID, gridParameters, hashMapData, g_hashCopyOffset, g_voxelData, g_previousVoxelData, g_constants.AccumulationFrames, g_constants.MaxStaleFrames);
	}
	else
	{
		SharcCopyHashEntry(dispatchThreadID, hashMapData, g_hashCopyOffset);
	}
}
