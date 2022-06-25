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

The ShaderBindingTable is a helper to construct the SBT. It helps to maintain the
proper offsets of each element, required when constructing the SBT, but also when filling the
dispatch rays description.

*/

#include "ShaderBindingTableGenerator.h"

#include <stdexcept>

// Helper to compute aligned buffer sizes
#ifndef ROUND_UP
#define ROUND_UP(v, powerOf2Alignment) (((v) + (powerOf2Alignment)-1) & ~((powerOf2Alignment)-1))
#endif

namespace nv_helpers_dx12
{

//--------------------------------------------------------------------------------------------------
//
// Add a ray generation program by name, with its list of data pointers or values according to
// the layout of its root signature
void ShaderBindingTableGenerator::AddRayGenerationProgram(const std::wstring& entryPoint,
                                                          const std::vector<void*>& inputData)
{
  m_rayGeneration.emplace_back(SBTEntry(entryPoint, inputData));

  m_rayGenerationEntrySize = GetEntrySize(m_rayGeneration);
}

//--------------------------------------------------------------------------------------------------
//
// Add a miss program by name, with its list of data pointers or values according to
// the layout of its root signature
void ShaderBindingTableGenerator::AddMissProgram(const std::wstring& entryPoint,
                                                 const std::vector<void*>& inputData)
{
  m_miss.emplace_back(SBTEntry(entryPoint, inputData));

  m_missEntrySize = GetEntrySize(m_miss);
}

//--------------------------------------------------------------------------------------------------
//
// Add a hit group by name, with its list of data pointers or values according to
// the layout of its root signature
void ShaderBindingTableGenerator::AddHitGroup(const std::wstring& entryPoint,
                                              const std::vector<void*>& inputData)
{
  m_hitGroup.emplace_back(SBTEntry(entryPoint, inputData));

  m_hitGroupEntrySize = GetEntrySize(m_hitGroup);
}

//--------------------------------------------------------------------------------------------------
//
// Compute the size of the SBT based on the set of programs and hit groups it contains
uint32_t ShaderBindingTableGenerator::ComputeSize() const
{
  // The total SBT size is the sum of the entries for ray generation, miss and hit groups, aligned
  // on 256 bytes
    return ROUND_UP(m_rayGenerationEntrySize * static_cast<UINT>(m_rayGeneration.size()) +
                    m_missEntrySize * static_cast<UINT>(m_miss.size()) +
                    m_hitGroupEntrySize * static_cast<UINT>(m_hitGroup.size()),
                    256);
}

//--------------------------------------------------------------------------------------------------
//
// Build the SBT and store it into sbtBuffer, which has to be pre-allocated on the upload heap.
// Access to the raytracing pipeline object is required to fetch program identifiers using their
// names
void ShaderBindingTableGenerator::Generate(ID3D12Resource* sbtBuffer,
                                           ID3D12StateObjectProperties* raytracingPipeline) const
{
  // Map the SBT
  uint8_t* pData;
  HRESULT hr = sbtBuffer->Map(0, nullptr, reinterpret_cast<void**>(&pData));
  if (FAILED(hr))
  {
    throw std::logic_error("Could not map the shader binding table");
  }
  // Copy the shader identifiers followed by their resource pointers or root constants: first the
  // ray generation, then the miss shaders, and finally the set of hit groups
  uint32_t offset;

  offset = CopyShaderData(raytracingPipeline, pData, m_rayGeneration, m_rayGenerationEntrySize);
  pData += offset;

  offset = CopyShaderData(raytracingPipeline, pData, m_miss, m_missEntrySize);
  pData += offset;

  offset = CopyShaderData(raytracingPipeline, pData, m_hitGroup, m_hitGroupEntrySize);

  // Unmap the SBT
  sbtBuffer->Unmap(0, nullptr);
}

//--------------------------------------------------------------------------------------------------
// The following getters are used to simplify the call to DispatchRays where the offsets of the
// shader programs must be exactly following the SBT layout

//--------------------------------------------------------------------------------------------------
//
// Get the size in bytes of the SBT section dedicated to ray generation programs
UINT ShaderBindingTableGenerator::GetRayGenerationSectionSize() const
{
  return m_rayGenerationEntrySize * static_cast<UINT>(m_rayGeneration.size());
}

//--------------------------------------------------------------------------------------------------
//
// Get the size in bytes of one ray generation program entry in the SBT
UINT ShaderBindingTableGenerator::GetRayGenerationEntrySize() const
{
  return m_rayGenerationEntrySize;
}

//--------------------------------------------------------------------------------------------------
//
// Get the size in bytes of the SBT section dedicated to miss programs
UINT ShaderBindingTableGenerator::GetMissSectionSize() const
{
  return m_missEntrySize * static_cast<UINT>(m_miss.size());
}

//--------------------------------------------------------------------------------------------------
//
// Get the size in bytes of one miss program entry in the SBT
UINT ShaderBindingTableGenerator::GetMissEntrySize() const
{
  return m_missEntrySize;
}

//--------------------------------------------------------------------------------------------------
//
// Get the size in bytes of the SBT section dedicated to hit groups
UINT ShaderBindingTableGenerator::GetHitGroupSectionSize() const
{
  return m_hitGroupEntrySize * static_cast<UINT>(m_hitGroup.size());
}

//--------------------------------------------------------------------------------------------------
//
// Get the size in bytes of one hit group entry in the SBT
UINT ShaderBindingTableGenerator::GetHitGroupEntrySize() const
{
  return m_hitGroupEntrySize;
}

//--------------------------------------------------------------------------------------------------
//
// For each entry, copy the shader identifier followed by its resource pointers and/or root
// constants in outputData, with a stride in bytes of entrySize, and returns the size in bytes
// actually written to outputData.
uint32_t ShaderBindingTableGenerator::CopyShaderData(
    ID3D12StateObjectProperties* raytracingPipeline, uint8_t* outputData,
    const std::vector<SBTEntry>& shaders, uint32_t entrySize)
{
  uint8_t* pData = outputData;
  for (const auto& shader : shaders)
  {
    // Get the shader identifier, and check whether that identifier is known
    void* id = raytracingPipeline->GetShaderIdentifier(shader.m_entryPoint.c_str());
    if (!id)
    {
      std::wstring errMsg(std::wstring(L"Unknown shader identifier used in the SBT: ") +
                          shader.m_entryPoint);

      const auto size = WideCharToMultiByte(CP_ACP, 0, &errMsg[0], static_cast<int>(errMsg.size()), nullptr, 0, nullptr, nullptr);
      std::string str(size, 0);
      WideCharToMultiByte(CP_ACP, 0, &errMsg[0], static_cast<int>(errMsg.size()), &str[0], size, nullptr, nullptr);

      throw std::logic_error(str);
    }
    // Copy the shader identifier
    memcpy(pData, id, ProgramIdSize);
    // Copy all its resources pointers or values in bulk
    memcpy(pData + ProgramIdSize, shader.m_inputData.data(), shader.m_inputData.size() * 8);

    pData += entrySize;
  }
  // Return the number of bytes actually written to the output buffer
  return static_cast<uint32_t>(shaders.size()) * entrySize;
}

//--------------------------------------------------------------------------------------------------
//
// Compute the size of the SBT entries for a set of entries, which is determined by the maximum
// number of parameters of their root signature
uint32_t ShaderBindingTableGenerator::GetEntrySize(const std::vector<SBTEntry>& entries)
{
  // Find the maximum number of parameters used by a single entry
  size_t maxArgs = 0;
  for (const auto& shader : entries)
  {
    maxArgs = max(maxArgs, shader.m_inputData.size());
  }
  // A SBT entry is made of a program ID and a set of parameters, taking 8 bytes each. Those
  // parameters can either be 8-bytes pointers, or 4-bytes constants
  uint32_t entrySize = ProgramIdSize + 8 * static_cast<uint32_t>(maxArgs);

  // The entries of the shader binding table must be 16-bytes-aligned
  entrySize = ROUND_UP(entrySize, D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT);

  return entrySize;
}

//--------------------------------------------------------------------------------------------------
//
//
ShaderBindingTableGenerator::SBTEntry::SBTEntry(std::wstring entryPoint,
                                                std::vector<void*> inputData)
    : m_entryPoint(std::move(entryPoint)), m_inputData(std::move(inputData))
{
}
} // namespace nv_helpers_dx12
