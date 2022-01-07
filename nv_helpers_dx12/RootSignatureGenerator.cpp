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


Utility class to create root signatures. The order in which the addition methods are called is
important as it will define the slots of the heap or of the Shader Binding Table to which buffer
pointers will be bound.

*/

#include "RootSignatureGenerator.h"

#include <stdexcept>

namespace nv_helpers_dx12
{

//--------------------------------------------------------------------------------------------------
//
// Add a root parameter to the shader.
void RootSignatureGenerator::AddRootParameter(const D3D12_ROOT_PARAMETER& parameter)
{
  m_parameters.push_back(parameter);
}

//--------------------------------------------------------------------------------------------------
//
// Create the root signature from the set of parameters, in the order of the addition calls
ID3D12RootSignature* RootSignatureGenerator::Generate(
    ID3D12Device* device,
    D3D12_ROOT_SIGNATURE_FLAGS flags /*= D3D12_ROOT_SIGNATURE_FLAG_NONE*/,
    const std::vector<D3D12_STATIC_SAMPLER_DESC>& staticSamplers /*= {}*/
)
{
  // Specify the root signature with its set of parameters
  D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
  rootDesc.NumParameters = static_cast<UINT>(m_parameters.size());
  rootDesc.pParameters = m_parameters.data();
  rootDesc.Flags = flags;
  rootDesc.pStaticSamplers = staticSamplers.data();
  rootDesc.NumStaticSamplers = static_cast<UINT>(staticSamplers.size());

  // Create the root signature from its descriptor
  ID3DBlob* pSigBlob;
  HRESULT hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &pSigBlob,
                                           nullptr);
  if (FAILED(hr))
  {
    throw std::logic_error("Cannot serialize root signature");
  }
  ID3D12RootSignature* pRootSig;
  hr = device->CreateRootSignature(0, pSigBlob->GetBufferPointer(), pSigBlob->GetBufferSize(),
                                   IID_PPV_ARGS(&pRootSig));
  if (FAILED(hr))
  {
    throw std::logic_error("Cannot create root signature");
  }
  pSigBlob->Release();
  return pRootSig;
}

} // namespace nv_helpers_dx12
