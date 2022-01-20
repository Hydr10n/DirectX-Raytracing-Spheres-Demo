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

Simple usage of this class:

pipeline.AddLibrary(m_rayGenLibrary.Get(), {L"RayGen"});
pipeline.AddLibrary(m_missLibrary.Get(), {L"Miss"});
pipeline.AddLibrary(m_hitLibrary.Get(), {L"ClosestHit"});

pipeline.AddHitGroup(L"HitGroup", L"ClosestHit");

pipeline.AddRootSignatureAssociation(m_rayGenSignature.Get(), {L"RayGen"});
pipeline.AddRootSignatureAssociation(m_missSignature.Get(), {L"Miss"});
pipeline.AddRootSignatureAssociation(m_hitSignature.Get(), {L"HitGroup"});

pipeline.SetMaxPayloadSize(4 * sizeof(float)); // RGB + distance

pipeline.SetMaxAttributeSize(2 * sizeof(float)); // barycentric coordinates

pipeline.SetMaxRecursionDepth(1);

rtStateObject = pipeline.Generate();

*/

#pragma once

#include <d3d12.h>

#include <string>
#include <vector>

namespace nv_helpers_dx12
{

/// Helper class to create raytracing pipelines
class RaytracingPipelineGenerator
{
public:
  /// Add a DXIL library to the pipeline. Note that this library has to be
  /// compiled with dxc, using a lib_6_3 target. The exported symbols must correspond exactly to the
  /// names of the shaders declared in the library, although unused ones can be omitted.
  void AddLibrary(const D3D12_SHADER_BYTECODE& dxilLibrary, const std::vector<std::wstring>& symbolExports);

  /// In DXR the hit-related shaders are grouped into hit groups. Such shaders are:
  /// - The intersection shader, which can be used to intersect custom geometry, and is called upon
  ///   hitting the bounding box the the object. A default one exists to intersect triangles
  /// - The any hit shader, called on each intersection, which can be used to perform early
  ///   alpha-testing and allow the ray to continue if needed. Default is a pass-through.
  /// - The closest hit shader, invoked on the hit point closest to the ray start.
  /// The shaders in a hit group share the same root signature, and are only referred to by the
  /// hit group name in other places of the program.
  void AddHitGroup(const std::wstring& hitGroupName, const std::wstring& closestHitSymbol,
                   const std::wstring& anyHitSymbol = L"",
                   const std::wstring& intersectionSymbol = L"");

  /// The shaders and hit groups may have various root signatures. This call associates a root
  /// signature to one or more symbols. All imported symbols must be associated to one root
  /// signature.
  void AddLocalRootSignatureAssociation(ID3D12RootSignature* rootSignature,
                                        const std::vector<std::wstring>& symbols);

  /// Compiles the raytracing state object
  ID3D12StateObject* Generate(
      ID3D12Device5* device,
      ID3D12RootSignature* globalRootSignature = nullptr,
      UINT maxTraceRecursionDepth = 1,
      UINT maxPayloadSizeInBytes = 0,
      UINT maxAttributeSizeInBytes = sizeof(float) * 2);

private:
  /// Storage for DXIL libraries and their exported symbols
  struct Library
  {
    Library(const D3D12_SHADER_BYTECODE& dxil, const std::vector<std::wstring>& exportedSymbols);

    Library(const Library& source);

    const std::vector<std::wstring> m_exportedSymbols;

    std::vector<D3D12_EXPORT_DESC> m_exports;
    D3D12_DXIL_LIBRARY_DESC m_libDesc;
  };

  /// Storage for the hit groups, binding the hit group name with the underlying intersection, any
  /// hit and closest hit symbols
  struct HitGroup
  {
    HitGroup(std::wstring hitGroupName, std::wstring closestHitSymbol,
             std::wstring anyHitSymbol = L"", std::wstring intersectionSymbol = L"");

    HitGroup(const HitGroup& source);

    std::wstring m_hitGroupName;
    std::wstring m_closestHitSymbol;
    std::wstring m_anyHitSymbol;
    std::wstring m_intersectionSymbol;
    D3D12_HIT_GROUP_DESC m_desc = {};
  };

  /// Storage for the association between shaders and root signatures
  struct RootSignatureAssociation
  {
    RootSignatureAssociation(ID3D12RootSignature* rootSignature,
                             const std::vector<std::wstring>& symbols);

    RootSignatureAssociation(const RootSignatureAssociation& source);

    ID3D12RootSignature* m_rootSignature;
    ID3D12RootSignature* m_rootSignaturePointer;
    std::vector<std::wstring> m_symbols;
    std::vector<LPCWSTR> m_symbolPointers;
    D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION m_association = {};
  };

  /// Build a list containing the export symbols for the ray generation shaders, miss shaders, and
  /// hit group names
  void BuildShaderExportList(std::vector<std::wstring>& exportedSymbols);

  std::vector<Library> m_libraries;
  std::vector<HitGroup> m_hitGroups;
  std::vector<RootSignatureAssociation> m_localRootSignatureAssociations;
};

} // namespace nv_helpers_dx12
