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

The raytracing pipeline combines the raytracing shaders into a state object,
that can be thought of as an executable GPU program. For that, it requires the
shaders compiled as DXIL libraries, where each library exports symbols in a way
similar to DLLs. Those symbols are then used to refer to these shaders libraries
when creating hit groups, associating the shaders to their root signatures and
declaring the steps of the pipeline. All the calls to this helper class can be
done in arbitrary order. Some basic sanity checks are also performed when
compiling in debug mode.

*/

#include "RaytracingPipelineGenerator.h"

#include <unordered_set>

#include <stdexcept>

namespace nv_helpers_dx12
{

//--------------------------------------------------------------------------------------------------
//
// Add a DXIL library to the pipeline. Note that this library has to be
// compiled with dxc, using a lib_6_3 target. The exported symbols must correspond exactly to the
// names of the shaders declared in the library, although unused ones can be omitted.
void RaytracingPipelineGenerator::AddLibrary(const D3D12_SHADER_BYTECODE& dxilLibrary,
                                             const std::vector<std::wstring>& symbolExports)
{
  m_libraries.emplace_back(Library(dxilLibrary, symbolExports));
}

//--------------------------------------------------------------------------------------------------
//
// In DXR the hit-related shaders are grouped into hit groups. Such shaders are:
// - The intersection shader, which can be used to intersect custom geometry, and is called upon
//   hitting the bounding box the the object. A default one exists to intersect triangles
// - The any hit shader, called on each intersection, which can be used to perform early
//   alpha-testing and allow the ray to continue if needed. Default is a pass-through.
// - The closest hit shader, invoked on the hit point closest to the ray start.
// The shaders in a hit group share the same root signature, and are only referred to by the
// hit group name in other places of the program.
void RaytracingPipelineGenerator::AddHitGroup(const std::wstring& hitGroupName,
                                              const std::wstring& closestHitSymbol,
                                              const std::wstring& anyHitSymbol /*= L""*/,
                                              const std::wstring& intersectionSymbol /*= L""*/)
{
  m_hitGroups.emplace_back(
      HitGroup(hitGroupName, closestHitSymbol, anyHitSymbol, intersectionSymbol));
}

//--------------------------------------------------------------------------------------------------
//
// The shaders and hit groups may have various root signatures. This call associates a root
// signature to one or more symbols. All imported symbols must be associated to one root
// signature.
void RaytracingPipelineGenerator::AddLocalRootSignatureAssociation(
    ID3D12RootSignature* rootSignature, const std::vector<std::wstring>& symbols)
{
  m_localRootSignatureAssociations.emplace_back(RootSignatureAssociation(rootSignature, symbols));
}

//--------------------------------------------------------------------------------------------------
//
// Compiles the raytracing state object
ID3D12StateObject* RaytracingPipelineGenerator::Generate(
    ID3D12Device5* device,
    ID3D12RootSignature* globalRootSignature /*= nullptr*/,
    UINT maxTraceRecursionDepth /*= 1*/,
    UINT maxPayloadSizeInBytes /*= 0*/,
    UINT maxAttributeSizeInBytes /*= sizeof(float) * 2*/)
{
  // The pipeline is made of a set of sub-objects, representing the DXIL libraries, hit group
  // declarations, root signature associations, plus some configuration objects
  size_t subobjectCount =
      m_libraries.size() +                          // DXIL libraries
      m_hitGroups.size() +                          // Hit group declarations
      1 +                                           // Shader configuration
      1 +                                           // Shader payload
      2 * m_localRootSignatureAssociations.size() + // Root signature declaration + association
      1 +                                           // Global root signature
      1;                                            // Final pipeline subobject

  // Initialize a vector with the target object count. It is necessary to make the allocation before
  // adding subobjects as some subobjects reference other subobjects by pointer. Using push_back and
  // emplace_back may reallocate the array and invalidate those pointers.
  std::vector<D3D12_STATE_SUBOBJECT> subobjects;
  subobjects.reserve(subobjectCount);

  // Add all the DXIL libraries
  for (const Library& lib : m_libraries)
  {
    D3D12_STATE_SUBOBJECT libSubobject;
    libSubobject.Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
    libSubobject.pDesc = &lib.m_libDesc;
    subobjects.emplace_back(libSubobject);
  }

  // Add all the hit group declarations
  for (const HitGroup& group : m_hitGroups)
  {
    D3D12_STATE_SUBOBJECT hitGroup;
    hitGroup.Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
    hitGroup.pDesc = &group.m_desc;
    subobjects.emplace_back(hitGroup);
  }

  // Add a subobject for the shader payload configuration
  D3D12_RAYTRACING_SHADER_CONFIG shaderDesc;
  shaderDesc.MaxPayloadSizeInBytes = maxPayloadSizeInBytes;
  shaderDesc.MaxAttributeSizeInBytes = maxAttributeSizeInBytes;

  D3D12_STATE_SUBOBJECT shaderConfigObject;
  shaderConfigObject.Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG;
  shaderConfigObject.pDesc = &shaderDesc;
  subobjects.emplace_back(shaderConfigObject);

  // Build a list of all the symbols for ray generation, miss and hit groups
  // Those shaders have to be associated with the payload definition
  std::vector<std::wstring> exportedSymbols;
  std::vector<LPCWSTR> exportedSymbolPointers;
  BuildShaderExportList(exportedSymbols);

  // Build an array of the string pointers
  exportedSymbolPointers.reserve(exportedSymbols.size());
  for (const auto& name : exportedSymbols)
  {
    exportedSymbolPointers.push_back(name.c_str());
  }
  const WCHAR** shaderExports = exportedSymbolPointers.data();

  // Add a subobject for the association between shaders and the payload
  D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION shaderPayloadAssociation;
  shaderPayloadAssociation.NumExports = static_cast<UINT>(exportedSymbols.size());
  shaderPayloadAssociation.pExports = shaderExports;

  // Associate the set of shaders with the payload defined in the previous subobject
  shaderPayloadAssociation.pSubobjectToAssociate = &subobjects.back();

  // Create and store the payload association object
  D3D12_STATE_SUBOBJECT shaderPayloadAssociationObject;
  shaderPayloadAssociationObject.Type = D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION;
  shaderPayloadAssociationObject.pDesc = &shaderPayloadAssociation;
  subobjects.emplace_back(shaderPayloadAssociationObject);

  // The root signature association requires two objects for each: one to declare the root
  // signature, and another to associate that root signature to a set of symbols
  for (RootSignatureAssociation& assoc : m_localRootSignatureAssociations)
  {
    // Add a subobject to declare the root signature
    D3D12_STATE_SUBOBJECT rootSigObject;
    rootSigObject.Type = D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE;
    rootSigObject.pDesc = &assoc.m_rootSignature;
    subobjects.emplace_back(rootSigObject);

    // Add a subobject for the association between the exported shader symbols and the root
    // signature
    assoc.m_association.NumExports = static_cast<UINT>(assoc.m_symbolPointers.size());
    assoc.m_association.pExports = assoc.m_symbolPointers.data();
    assoc.m_association.pSubobjectToAssociate = &subobjects.back();

    D3D12_STATE_SUBOBJECT rootSigAssociationObject;
    rootSigAssociationObject.Type = D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION;
    rootSigAssociationObject.pDesc = &assoc.m_association;
    subobjects.emplace_back(rootSigAssociationObject);
  }

  // The pipeline construction always requires an empty global root signature
  D3D12_STATE_SUBOBJECT globalRootSig;
  globalRootSig.Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
  globalRootSig.pDesc = &globalRootSignature;
  subobjects.emplace_back(globalRootSig);

  // Add a subobject for the ray tracing pipeline configuration
  D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig;
  pipelineConfig.MaxTraceRecursionDepth = maxTraceRecursionDepth;

  D3D12_STATE_SUBOBJECT pipelineConfigObject;
  pipelineConfigObject.Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;
  pipelineConfigObject.pDesc = &pipelineConfig;
  subobjects.emplace_back(pipelineConfigObject);

  // Describe the ray tracing pipeline state object
  D3D12_STATE_OBJECT_DESC pipelineDesc;
  pipelineDesc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
  pipelineDesc.NumSubobjects = static_cast<UINT>(subobjects.size());
  pipelineDesc.pSubobjects = subobjects.data();

  ID3D12StateObject* rtStateObject;
  HRESULT hr = device->CreateStateObject(&pipelineDesc, IID_PPV_ARGS(&rtStateObject));
  if (FAILED(hr))
  {
    throw std::logic_error("Could not create the raytracing state object");
  }
  return rtStateObject;
}

//--------------------------------------------------------------------------------------------------
//
// Build a list containing the export symbols for the ray generation shaders, miss shaders, and
// hit group names
void RaytracingPipelineGenerator::BuildShaderExportList(std::vector<std::wstring>& exportedSymbols)
{
  // Get all names from libraries
  // Get names associated to hit groups
  // Return list of libraries+hit group names - shaders in hit groups

  std::unordered_set<std::wstring> exports;

  // Add all the symbols exported by the libraries
  for (const Library& lib : m_libraries)
  {
    for (const auto& exportName : lib.m_exportedSymbols)
    {
#ifdef _DEBUG
      // Sanity check in debug mode: check that no name is exported more than once
      if (exports.contains(exportName))
      {
        throw std::logic_error("Multiple definition of a symbol in the imported DXIL libraries");
      }
#endif
      exports.insert(exportName);
    }
  }

#ifdef _DEBUG
  // Sanity check in debug mode: verify that the hit groups do not reference an unknown shader name
  std::unordered_set<std::wstring> allExports = exports;

  for (const auto& hitGroup : m_hitGroups)
  {
    if (!hitGroup.m_anyHitSymbol.empty() && !exports.contains(hitGroup.m_anyHitSymbol))
    {
      throw std::logic_error("Any hit symbol not found in the imported DXIL libraries");
    }

    if (!hitGroup.m_closestHitSymbol.empty() && !exports.contains(hitGroup.m_closestHitSymbol))
    {
      throw std::logic_error("Closest hit symbol not found in the imported DXIL libraries");
    }

    if (!hitGroup.m_intersectionSymbol.empty() && !exports.contains(hitGroup.m_intersectionSymbol))
    {
      throw std::logic_error("Intersection symbol not found in the imported DXIL libraries");
    }

    allExports.insert(hitGroup.m_hitGroupName);
  }

  // Sanity check in debug mode: verify that the root signature associations do not reference an
  // unknown shader or hit group name
  for (const auto& assoc : m_localRootSignatureAssociations)
  {
    for (const auto& symb : assoc.m_symbols)
    {
      if (!symb.empty() && !allExports.contains(symb))
      {
        throw std::logic_error("Root association symbol not found in the "
                               "imported DXIL libraries and hit group names");
      }
    }
  }
#endif

  // Go through all hit groups and remove the symbols corresponding to intersection, any hit and
  // closest hit shaders from the symbol set
  for (const auto& hitGroup : m_hitGroups)
  {
    if (!hitGroup.m_anyHitSymbol.empty())
    {
      exports.erase(hitGroup.m_anyHitSymbol);
    }
    if (!hitGroup.m_closestHitSymbol.empty())
    {
      exports.erase(hitGroup.m_closestHitSymbol);
    }
    if (!hitGroup.m_intersectionSymbol.empty())
    {
      exports.erase(hitGroup.m_intersectionSymbol);
    }
    exports.insert(hitGroup.m_hitGroupName);
  }

  // Finally build a vector containing ray generation and miss shaders, plus the hit group names
  for (const auto& name : exports)
  {
    exportedSymbols.push_back(name);
  }
}

//--------------------------------------------------------------------------------------------------
//
// Store data related to a DXIL library: the library itself, the exported symbols, and the
// associated descriptors
RaytracingPipelineGenerator::Library::Library(const D3D12_SHADER_BYTECODE& dxil,
                                              const std::vector<std::wstring>& exportedSymbols)
    : m_exportedSymbols(exportedSymbols), m_exports(exportedSymbols.size())
{
  // Create one export descriptor per symbol
  for (size_t i = 0; i < m_exportedSymbols.size(); i++)
  {
    m_exports[i].Name = m_exportedSymbols[i].c_str();
    m_exports[i].ExportToRename = nullptr;
    m_exports[i].Flags = D3D12_EXPORT_FLAG_NONE;
  }

  // Create a library descriptor combining the DXIL code and the export names
  m_libDesc.DXILLibrary = dxil;
  m_libDesc.NumExports = static_cast<UINT>(m_exportedSymbols.size());
  m_libDesc.pExports = m_exports.data();
}

//--------------------------------------------------------------------------------------------------
//
// This copy constructor has to be defined so that the export descriptors are set correctly. Using
// the default constructor would copy the string pointers of the symbols into the descriptors, which
// would cause issues when the original Library object gets out of scope
RaytracingPipelineGenerator::Library::Library(const Library& source)
    : Library(source.m_libDesc.DXILLibrary, source.m_exportedSymbols)
{
}

//--------------------------------------------------------------------------------------------------
//
// Create a hit group descriptor from the input hit group name and shader symbols
RaytracingPipelineGenerator::HitGroup::HitGroup(std::wstring hitGroupName,
                                                std::wstring closestHitSymbol,
                                                std::wstring anyHitSymbol /*= L""*/,
                                                std::wstring intersectionSymbol /*= L""*/)
    : m_hitGroupName(std::move(hitGroupName)), m_closestHitSymbol(std::move(closestHitSymbol)),
      m_anyHitSymbol(std::move(anyHitSymbol)), m_intersectionSymbol(std::move(intersectionSymbol))
{
  // Indicate which shader program is used for closest hit, leave the other
  // ones undefined (default behavior), export the name of the group
  m_desc.HitGroupExport = m_hitGroupName.c_str();
  m_desc.ClosestHitShaderImport = m_closestHitSymbol.empty() ? nullptr : m_closestHitSymbol.c_str();
  m_desc.AnyHitShaderImport = m_anyHitSymbol.empty() ? nullptr : m_anyHitSymbol.c_str();
  m_desc.IntersectionShaderImport =
      m_intersectionSymbol.empty() ? nullptr : m_intersectionSymbol.c_str();
}

//--------------------------------------------------------------------------------------------------
//
// This copy constructor has to be defined so that the export descriptors are set correctly. Using
// the default constructor would copy the string pointers of the symbols into the descriptors, which
// would cause issues when the original HitGroup object gets out of scope
RaytracingPipelineGenerator::HitGroup::HitGroup(const HitGroup& source)
    : HitGroup(source.m_hitGroupName, source.m_closestHitSymbol, source.m_anyHitSymbol,
               source.m_intersectionSymbol)
{
}

//--------------------------------------------------------------------------------------------------
//
// Store the association between a set of symbols and a root signature. The associated descriptors
// will be built when compiling the pipeline. We store the symbol pointers directly so that they can
// be used without processing during compilation.
RaytracingPipelineGenerator::RootSignatureAssociation::RootSignatureAssociation(
    ID3D12RootSignature* rootSignature, const std::vector<std::wstring>& symbols)
    : m_rootSignature(rootSignature), m_symbols(symbols), m_symbolPointers(symbols.size())
{
  for (size_t i = 0; i < m_symbols.size(); i++)
  {
    m_symbolPointers[i] = m_symbols[i].c_str();
  }
  m_rootSignaturePointer = m_rootSignature;
}

//--------------------------------------------------------------------------------------------------
//
// This copy constructor has to be defined so that the export descriptors are set correctly. Using
// the default constructor would copy the string pointers of the symbols into the descriptors, which
// would cause issues when the original RootSignatureAssociation object gets out of scope
RaytracingPipelineGenerator::RootSignatureAssociation::RootSignatureAssociation(
    const RootSignatureAssociation& source)
    : RootSignatureAssociation(source.m_rootSignature, source.m_symbols)
{
}
} // namespace nv_helpers_dx12
