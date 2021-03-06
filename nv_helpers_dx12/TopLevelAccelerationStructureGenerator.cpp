/*-----------------------------------------------------------------------
Copyright (c) 2014-2018, NVIDIA. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
* Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.
* Neither the name of its contributors may be used to endorse
or promote products derived from this software without specific
prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
-----------------------------------------------------------------------*/

/*
Contacts for feedback:
- pgautron@nvidia.com (Pascal Gautron)
- mlefrancois@nvidia.com (Martin-Karl Lefrancois)

The top-level hierarchy is used to store a set of instances represented by
bottom-level hierarchies in a way suitable for fast intersection at runtime. To
be built, this data structure requires some scratch space which has to be
allocated by the application. Similarly, the resulting data structure is stored
in an application-controlled buffer.

To be used, the application must first add all the instances to be contained in
the final structure, using AddInstance. After all instances have been added,
ComputeASBufferSizes will prepare the build, and provide the required sizes for
the scratch data and the final result. The Build call will finally compute the
acceleration structure and store it in the result buffer.

Note that the build is enqueued in the command list, meaning that the scratch
buffer needs to be kept until the command list execution is finished.

*/

#include "TopLevelAccelerationStructureGenerator.h"

#include <stdexcept>

// Helper to compute aligned buffer sizes
#ifndef ROUND_UP
#define ROUND_UP(v, powerOf2Alignment) (((v) + (powerOf2Alignment)-1) & ~((powerOf2Alignment)-1))
#endif

namespace nv_helpers_dx12
{

//--------------------------------------------------------------------------------------------------
//
// Add an instance decriptor to the top-level acceleration structure.
void TopLevelAccelerationStructureGenerator::AddInstance(
    D3D12_GPU_VIRTUAL_ADDRESS bottomLevelAS, // Bottom-level acceleration structure containing the
                                             // actual geometric data of the instance
    DirectX::FXMMATRIX transform,            // Transform matrix to apply to the instance, allowing
                                             // the same bottom-level AS to be used at several
                                             // world-space positions
    UINT ID,                                 // Instance ID, which can be used in the shaders to
                                             // identify this specific instance
    UINT hitGroupIndex,                      // Hit group index, corresponding the the index of the
                                             // hit group in the Shader Binding Table that will be
                                             // invocated upon hitting the geometry
    UINT mask                                /*= ~0*/,
    D3D12_RAYTRACING_INSTANCE_FLAGS flags    /*= D3D12_RAYTRACING_INSTANCE_FLAG_NONE*/
)
{
  D3D12_RAYTRACING_INSTANCE_DESC instance;
  DirectX::XMStoreFloat3x4(reinterpret_cast<DirectX::XMFLOAT3X4*>(instance.Transform), transform);
  instance.InstanceID = ID;
  instance.InstanceMask = mask;
  instance.InstanceContributionToHitGroupIndex = hitGroupIndex;
  instance.Flags = flags;
  instance.AccelerationStructure = bottomLevelAS;
  m_instanceDescs.emplace_back(instance);
}

//--------------------------------------------------------------------------------------------------
//
// Compute the size of the scratch space required to build the acceleration
// structure, as well as the size of the resulting structure. The allocation of
// the buffers is then left to the application
void TopLevelAccelerationStructureGenerator::ComputeBufferSizes(
    ID3D12Device5* device, // Device on which the build will be performed
    UINT64& scratchSizeInBytes,              // Required scratch memory on the GPU to build
                                             // the acceleration structure
    UINT64& resultSizeInBytes,               // Required GPU memory to store the acceleration
                                             // structure
    UINT64& instanceDescsSizeInBytes         // Required GPU memory to store instance
                                             // descriptors, containing the matrices,
                                             // indices etc.
)
{
  // Describe the work being requested, in this case the construction of a
  // (possibly dynamic) top-level hierarchy, with the given instance descriptors
  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS
  prebuildDesc = {};
  prebuildDesc.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
  prebuildDesc.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
  prebuildDesc.NumDescs = static_cast<UINT>(m_instanceDescs.size());
  prebuildDesc.Flags = m_flags;

  // This structure is used to hold the sizes of the required scratch memory and
  // resulting AS
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};

  // Building the acceleration structure (AS) requires some scratch space, as
  // well as space to store the resulting structure This function computes a
  // conservative estimate of the memory requirements for both, based on the
  // number of bottom-level instances.
  device->GetRaytracingAccelerationStructurePrebuildInfo(&prebuildDesc, &info);

  // Buffer sizes need to be 256-byte-aligned
  info.ResultDataMaxSizeInBytes =
      ROUND_UP(info.ResultDataMaxSizeInBytes, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
  info.ScratchDataSizeInBytes =
      ROUND_UP(info.ScratchDataSizeInBytes, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

  m_resultSizeInBytes = info.ResultDataMaxSizeInBytes;
  m_scratchSizeInBytes = info.ScratchDataSizeInBytes;
  // The instance descriptors are stored as-is in GPU memory, so we can deduce
  // the required size from the instance count
  m_instanceDescsSizeInBytes =
      ROUND_UP(sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * prebuildDesc.NumDescs,
               D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

  scratchSizeInBytes = m_scratchSizeInBytes;
  resultSizeInBytes = m_resultSizeInBytes;
  instanceDescsSizeInBytes = m_instanceDescsSizeInBytes;
}

//--------------------------------------------------------------------------------------------------
//
// Enqueue the construction of the acceleration structure on a command list,
// using application-provided buffers and possibly a pointer to the previous
// acceleration structure in case of iterative updates. Note that the update can
// be done in place: the result and previousResult pointers can be the same.
void TopLevelAccelerationStructureGenerator::Generate(
    ID3D12GraphicsCommandList4* commandList, // Command list on which the build will be enqueued
    ID3D12Resource* scratchBuffer,     // Scratch buffer used by the builder to
                                       // store temporary data
    ID3D12Resource* resultBuffer,      // Result buffer storing the acceleration structure
    ID3D12Resource* descriptorsBuffer, // Auxiliary result buffer containing the instance
                                       // descriptors, has to be in upload heap
    ID3D12Resource* previousResult /*= nullptr*/ // Optional previous acceleration
                                                 // structure, used if an iterative update
                                                 // is requested
) const
{
  // Copy the descriptors in the target descriptor buffer
  D3D12_RAYTRACING_INSTANCE_DESC* instanceDescs;
  descriptorsBuffer->Map(0, nullptr, reinterpret_cast<void**>(&instanceDescs));
  if (!instanceDescs)
  {
    throw std::logic_error("Cannot map the instance descriptor buffer");
  }

  std::copy(m_instanceDescs.cbegin(), m_instanceDescs.cend(), instanceDescs);

  descriptorsBuffer->Unmap(0, nullptr);

  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS flags = m_flags;
  // The stored flags represent whether the AS has been built for updates or
  // not. If yes and an update is requested, the builder is told to only update
  // the AS instead of fully rebuilding it
  if (flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE
      && previousResult != nullptr)
  {
    flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
  }

  // Create a descriptor of the requested builder work, to generate a top-level
  // AS from the input parameters
  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc;
  buildDesc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
  buildDesc.Inputs.Flags = flags;
  buildDesc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
  buildDesc.Inputs.InstanceDescs = descriptorsBuffer->GetGPUVirtualAddress();
  buildDesc.Inputs.NumDescs = static_cast<UINT>(m_instanceDescs.size());
  buildDesc.DestAccelerationStructureData = {resultBuffer->GetGPUVirtualAddress()
                                             };
  buildDesc.ScratchAccelerationStructureData = {scratchBuffer->GetGPUVirtualAddress()
                                                };
  buildDesc.SourceAccelerationStructureData =
      previousResult ? previousResult->GetGPUVirtualAddress() : 0;

  // Build the top-level AS
  commandList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

  // Wait for the builder to complete by setting a barrier on the resulting
  // buffer. This can be important in case the rendering is triggered
  // immediately afterwards, without executing the command list
  D3D12_RESOURCE_BARRIER uavBarrier;
  uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  uavBarrier.UAV.pResource = resultBuffer;
  uavBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  commandList->ResourceBarrier(1, &uavBarrier);
}

} // namespace nv_helpers_dx12
