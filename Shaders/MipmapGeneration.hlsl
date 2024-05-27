/***************************************************************************
 # Copyright (c) 2021-2023, NVIDIA CORPORATION.  All rights reserved.
 #
 # NVIDIA CORPORATION and its licensors retain all intellectual property
 # and proprietary rights in and to this software, related documentation
 # and any modifications thereto.  Any use, reproduction, disclosure or
 # distribution of this software and related documentation without an express
 # license agreement from NVIDIA CORPORATION is strictly prohibited.
 **************************************************************************/

#include "rtxdi/RtxdiMath.hlsli"

struct Constants
{
	uint MipLevel, MipLevels;
};
ConstantBuffer<Constants> g_constants;

groupshared float s_weights[16];

#define TEXTURE(offset) RWTexture2D<float> texture = ResourceDescriptorHeap[g_constants.MipLevel + offset];

[RootSignature(
	"RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED),"
	"RootConstants(num32BitConstants=2, b0)"
)]
// Warning: do not change the group size. The algorithm is hardcoded to process 16x16 tiles.
[numthreads(256, 1, 1)]
void main(uint2 GroupIndex : SV_GroupID, uint ThreadIndex : SV_GroupThreadID)
{
	uint2 LocalIndex = RTXDI_LinearIndexToZCurve(ThreadIndex);
	uint2 GlobalIndex = (GroupIndex * 16) + LocalIndex;

	// Step 0: Load a 2x2 quad of pixels from the source texture or the source mip level.
	float4 sourceWeights;
	{
		uint2 sourcePos = GlobalIndex.xy * 2;
		
		TEXTURE(0);
		sourceWeights.x = texture[sourcePos + int2(0, 0)];
		sourceWeights.y = texture[sourcePos + int2(0, 1)];
		sourceWeights.z = texture[sourcePos + int2(1, 0)];
		sourceWeights.w = texture[sourcePos + int2(1, 1)];
	}

	uint mipLevelsToWrite = g_constants.MipLevels - g_constants.MipLevel - 1;
	if (mipLevelsToWrite < 1)
		return;

	// Average those weights and write out the first mip.
	float weight = (sourceWeights.x + sourceWeights.y + sourceWeights.z + sourceWeights.w) * 0.25;

	{
		TEXTURE(1);
		texture[GlobalIndex.xy] = weight;
	}

	if (mipLevelsToWrite < 2)
		return;

	// The following sequence is an optimized hierarchical downsampling algorithm using wave ops.
	// It assumes that the wave size is at least 16 lanes, which is true for both NV and AMD GPUs.
	// It also assumes that the threads are laid out in the group using the Z-curve pattern.

	// Step 1: Average 2x2 groups of pixels.
	uint lane = WaveGetLaneIndex();
	weight = (weight
		+ WaveReadLaneAt(weight, lane + 1)
		+ WaveReadLaneAt(weight, lane + 2)
		+ WaveReadLaneAt(weight, lane + 3)) * 0.25;

	if ((lane & 3) == 0)
	{
		TEXTURE(2);
		texture[GlobalIndex.xy >> 1] = weight;
	}

	if (mipLevelsToWrite < 3)
		return;

	// Step 2: Average the previous results from 2 pixels away.
	weight = (weight
		+ WaveReadLaneAt(weight, lane + 4)
		+ WaveReadLaneAt(weight, lane + 8)
		+ WaveReadLaneAt(weight, lane + 12)) * 0.25;

	if ((lane & 15) == 0)
	{
		TEXTURE(3);
		texture[GlobalIndex.xy >> 2] = weight;

		// Store the intermediate result into shared memory.
		s_weights[ThreadIndex >> 4] = weight;
	}

	if (mipLevelsToWrite < 4)
		return;

	GroupMemoryBarrierWithGroupSync();

	// The rest operates on a 4x4 group of values for the entire thread group
	if (ThreadIndex >= 16)
		return;

	// Load the intermediate results
	weight = s_weights[ThreadIndex];

	// Change the output texture addressing because we'll be only writing a 2x2 block of pixels
	GlobalIndex = (GroupIndex * 2) + (LocalIndex >> 1);

	// Step 3: Average the previous results from adjacent threads, meaning from 4 pixels away.
	weight = (weight
		+ WaveReadLaneAt(weight, lane + 1)
		+ WaveReadLaneAt(weight, lane + 2)
		+ WaveReadLaneAt(weight, lane + 3)) * 0.25;

	if ((lane & 3) == 0)
	{
		TEXTURE(4);
		texture[GlobalIndex.xy] = weight;
	}

	if (mipLevelsToWrite < 5)
		return;

	// Step 4: Average the previous results from 8 pixels away.
	weight = (weight
		+ WaveReadLaneAt(weight, lane + 4)
		+ WaveReadLaneAt(weight, lane + 8)
		+ WaveReadLaneAt(weight, lane + 12)) * 0.25;

	if (lane == 0)
	{
		TEXTURE(5);
		texture[GlobalIndex.xy >> 1] = weight;
	}
}