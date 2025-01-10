#include "SharcCommon.h"

#include "Camera.hlsli"

struct Constants
{
	bool IsResolve;
	uint Capacity, AccumulationFrames, MaxStaleFrames;
	float SceneScale;
	bool IsAntiFireflyEnabled;
};
ConstantBuffer<Constants> g_constants : register(b0);

ConstantBuffer<Camera> g_camera : register(b1);

RWStructuredBuffer<uint64_t> g_hashEntries : register(u0);
RWStructuredBuffer<uint> g_hashCopyOffset : register(u1);
RWStructuredBuffer<uint4> g_previousVoxelData : register(u2);
RWStructuredBuffer<uint4> g_voxelData : register(u3);

[RootSignature(
	"RootConstants(num32BitConstants=6, b0),"
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
		SharcParameters sharcParameters;
		sharcParameters.gridParameters.cameraPosition = g_camera.Position;
		sharcParameters.gridParameters.sceneScale = g_constants.SceneScale;
		sharcParameters.gridParameters.logarithmBase = SHARC_GRID_LOGARITHM_BASE;
		sharcParameters.gridParameters.levelBias = SHARC_GRID_LEVEL_BIAS;
		sharcParameters.hashMapData = hashMapData;
		sharcParameters.voxelDataBuffer = g_voxelData;
		sharcParameters.voxelDataBufferPrev = g_previousVoxelData;

		SharcResolveParameters resolveParameters;
		resolveParameters.cameraPositionPrev = g_camera.PreviousPosition;
		resolveParameters.accumulationFrameNum = g_constants.AccumulationFrames;
		resolveParameters.staleFrameNumMax = g_constants.MaxStaleFrames;
		resolveParameters.enableAntiFireflyFilter = g_constants.IsAntiFireflyEnabled;

		SharcResolveEntry(dispatchThreadID, sharcParameters, resolveParameters
#if SHARC_DEFERRED_HASH_COMPACTION
			, g_hashCopyOffset
#endif
		);
	}
	else
	{
		SharcCopyHashEntry(dispatchThreadID, hashMapData, g_hashCopyOffset);
	}
}
