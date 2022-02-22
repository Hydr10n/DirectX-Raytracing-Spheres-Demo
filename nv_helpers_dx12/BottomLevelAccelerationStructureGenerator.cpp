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
*/

#include "BottomLevelAccelerationStructureGenerator.h"

#include <stdexcept>

// Helper to compute aligned buffer sizes
#ifndef ROUND_UP
#define ROUND_UP(v, powerOf2Alignment)                                         \
  (((v) + (powerOf2Alignment)-1) & ~((powerOf2Alignment)-1))
#endif

namespace nv_helpers_dx12 {

//--------------------------------------------------------------------------------------------------
// Add a geometry descriptor into the acceleration structure.
void BottomLevelAccelerationStructureGenerator::AddGeometry(
    const D3D12_RAYTRACING_GEOMETRY_DESC& geometry
) {
    m_geometryDescs.push_back(geometry);
}

//--------------------------------------------------------------------------------------------------
// Compute the size of the scratch space required to build the acceleration
// structure, as well as the size of the resulting structure. The allocation of
// the buffers is then left to the application
void BottomLevelAccelerationStructureGenerator::ComputeASBufferSizes(
    ID3D12Device5 *device, // Device on which the build will be performed
    UINT64 &scratchSizeInBytes, // Required scratch memory on the GPU to build
                                // the acceleration structure
    UINT64 &resultSizeInBytes   // Required GPU memory to store the acceleration
                                // structure
) {
  // Describe the work being requested, in this case the construction of a
  // (possibly dynamic) bottom-level hierarchy, with the given vertex buffers
  
  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS prebuildDesc;
  prebuildDesc.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
  prebuildDesc.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
  prebuildDesc.NumDescs = static_cast<UINT>(m_geometryDescs.size());
  prebuildDesc.pGeometryDescs = m_geometryDescs.data();
  prebuildDesc.Flags = m_flags;

  // This structure is used to hold the sizes of the required scratch memory and
  // resulting AS
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};

  // Building the acceleration structure (AS) requires some scratch space, as
  // well as space to store the resulting structure This function computes a
  // conservative estimate of the memory requirements for both, based on the
  // geometry size.
  device->GetRaytracingAccelerationStructurePrebuildInfo(&prebuildDesc, &info);

  // Buffer sizes need to be 256-byte-aligned
  m_scratchSizeInBytes =
      ROUND_UP(info.ScratchDataSizeInBytes,
               D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
  m_resultSizeInBytes = ROUND_UP(info.ResultDataMaxSizeInBytes,
                                D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

  scratchSizeInBytes = m_scratchSizeInBytes;
  resultSizeInBytes = m_resultSizeInBytes;
}

//--------------------------------------------------------------------------------------------------
// Enqueue the construction of the acceleration structure on a command list,
// using application-provided buffers and possibly a pointer to the previous
// acceleration structure in case of iterative updates. Note that the update can
// be done in place: the result and previousResult pointers can be the same.
void BottomLevelAccelerationStructureGenerator::Generate(
    ID3D12GraphicsCommandList4
        *commandList, // Command list on which the build will be enqueued
    ID3D12Resource *scratchBuffer, // Scratch buffer used by the builder to
                                   // store temporary data
    ID3D12Resource
        *resultBuffer, // Result buffer storing the acceleration structure
    ID3D12Resource *previousResult // Optional previous acceleration
                                   // structure, used if an iterative update
                                   // is requested
) const {
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS flags = m_flags;
  // The stored flags represent whether the AS has been built for updates or
  // not. If yes and an update is requested, the builder is told to only update
  // the AS instead of fully rebuilding it
  if (flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE
      && previousResult != nullptr) {
    flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
  }

  if (m_resultSizeInBytes == 0 || m_scratchSizeInBytes == 0) {
    throw std::logic_error(
        "Invalid scratch and result buffer sizes - ComputeASBufferSizes needs "
        "to be called before Build");
  }
  // Create a descriptor of the requested builder work, to generate a
  // bottom-level AS from the input parameters
  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc;
  buildDesc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
  buildDesc.Inputs.Flags = flags;
  buildDesc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
  buildDesc.Inputs.NumDescs = static_cast<UINT>(m_geometryDescs.size());
  buildDesc.Inputs.pGeometryDescs = m_geometryDescs.data();
  buildDesc.DestAccelerationStructureData = {
      resultBuffer->GetGPUVirtualAddress()};
  buildDesc.ScratchAccelerationStructureData = {
      scratchBuffer->GetGPUVirtualAddress()};
  buildDesc.SourceAccelerationStructureData =
      previousResult ? previousResult->GetGPUVirtualAddress() : 0;

  // Build the AS
  commandList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

  // Wait for the builder to complete by setting a barrier on the resulting
  // buffer. This is particularly important as the construction of the top-level
  // hierarchy may be called right afterwards, before executing the command
  // list.
  D3D12_RESOURCE_BARRIER uavBarrier;
  uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  uavBarrier.UAV.pResource = resultBuffer;
  uavBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  commandList->ResourceBarrier(1, &uavBarrier);
}
} // namespace nv_helpers_dx12
