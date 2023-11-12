//--------------------------------------------------------------------------------------
// File: RenderTexture.ixx
//
// Helper for managing offscreen render targets
//
// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
//-------------------------------------------------------------------------------------

module;

#include "pch.h"

#include "directxtk12/DirectXHelpers.h"
#include "directxtk12/DescriptorHeap.h"

export module RenderTexture;

using namespace DirectX;
using namespace std;

export namespace DX
{
    class RenderTexture
    {
    public:
        RenderTexture(RenderTexture&&) = default;
        RenderTexture& operator= (RenderTexture&&) = default;

        RenderTexture(RenderTexture const&) = delete;
        RenderTexture& operator= (RenderTexture const&) = delete;

        explicit RenderTexture(DXGI_FORMAT format) noexcept :
            m_device{},
            m_state(D3D12_RESOURCE_STATE_COMMON),
            m_srvDescriptor{},
            m_uavDescriptor{},
            m_rtvDescriptor{},
            m_srvDescriptorHeapIndex(~0u),
            m_uavDescriptorHeapIndex(~0u),
            m_rtvDescriptorHeapIndex(~0u),
            m_clearColor{},
            m_format(format),
            m_width{},
            m_height{}
        {
        }

        void SetDevice(_In_ ID3D12Device* device,
            const DescriptorHeap* resourceDescriptorHeap = nullptr, UINT srvDescriptorHeapIndex = ~0u, UINT uavDescriptorHeapIndex = ~0u,
            const DescriptorHeap* renderDescriptorHeap = nullptr, UINT rtvDescriptorHeapIndex = ~0u)
        {
            {
                D3D12_FEATURE_DATA_FORMAT_SUPPORT formatSupport = { m_format, D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE };
                if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &formatSupport, sizeof(formatSupport))))
                {
                    throw runtime_error("CheckFeatureSupport");
                }

                constexpr UINT required = D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_RENDER_TARGET;
                if ((formatSupport.Support1 & required) != required)
                {
#ifdef _DEBUG
                    char buff[128] = {};
                    sprintf_s(buff, "RenderTexture: Device does not support the requested format (%u)!\n", m_format);
                    OutputDebugStringA(buff);
#endif
                    throw runtime_error("RenderTexture");
                }
            }

            m_device = device;

            if (srvDescriptorHeapIndex != ~0u)
            {
                if (resourceDescriptorHeap == nullptr)
                {
                    throw runtime_error("RenderTexture");
                }

                m_srvDescriptor = resourceDescriptorHeap->GetCpuHandle(srvDescriptorHeapIndex);
            }

            if (uavDescriptorHeapIndex != ~0u)
            {
                if (resourceDescriptorHeap == nullptr)
                {
                    throw runtime_error("RenderTexture");
                }

                m_uavDescriptor = resourceDescriptorHeap->GetCpuHandle(uavDescriptorHeapIndex);
            }

            if (rtvDescriptorHeapIndex != ~0u)
            {
                if (renderDescriptorHeap == nullptr)
                {
                    throw runtime_error("RenderTexture");
                }

                m_rtvDescriptor = renderDescriptorHeap->GetCpuHandle(rtvDescriptorHeapIndex);
            }

            m_srvDescriptorHeapIndex = srvDescriptorHeapIndex;
            m_uavDescriptorHeapIndex = uavDescriptorHeapIndex;
            m_rtvDescriptorHeapIndex = rtvDescriptorHeapIndex;
        }

        void CreateResource(UINT width, UINT height)
        {
            if (width == m_width && height == m_height)
                return;

            if (!m_device)
                return;

            auto const heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

            const auto flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | (m_uavDescriptor.ptr ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE);
            const D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Tex2D(m_format,
                width,
                height,
                1, 1, 1, 0, flags);

            D3D12_CLEAR_VALUE clearValue = { m_format, {} };
            memcpy(clearValue.Color, m_clearColor, sizeof(clearValue.Color));

            m_state = D3D12_RESOURCE_STATE_RENDER_TARGET;

            // Create a render target
            ThrowIfFailed(
                m_device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES,
                    &desc,
                    m_state, &clearValue,
                    IID_PPV_ARGS(&m_resource))
            );

            SetDebugObjectName(m_resource.Get(), L"RenderTexture");

            // Create RTV.
            if (m_rtvDescriptor.ptr)
            {
                m_device->CreateRenderTargetView(m_resource.Get(), nullptr, m_rtvDescriptor);
            }

            // Create UAV.
            if (m_uavDescriptor.ptr)
            {
                m_device->CreateUnorderedAccessView(m_resource.Get(), nullptr, nullptr, m_uavDescriptor);
            }

            // Create SRV.
            if (m_srvDescriptor.ptr)
            {
                m_device->CreateShaderResourceView(m_resource.Get(), nullptr, m_srvDescriptor);
            }

            m_width = width;
            m_height = height;
        }

        void TransitionTo(_In_ ID3D12GraphicsCommandList* commandList, D3D12_RESOURCE_STATES afterState)
        {
            TransitionResource(commandList, m_resource.Get(), m_state, afterState);
            m_state = afterState;
        }

        void BeginScene(_In_ ID3D12GraphicsCommandList* commandList)
        {
            TransitionTo(commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);
        }

        void EndScene(_In_ ID3D12GraphicsCommandList* commandList)
        {
            TransitionTo(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }

        void Clear(_In_ ID3D12GraphicsCommandList* commandList)
        {
            commandList->ClearRenderTargetView(m_rtvDescriptor, m_clearColor, 0, nullptr);
        }

        void SetClearColor(FXMVECTOR color)
        {
            XMStoreFloat4(reinterpret_cast<XMFLOAT4*>(m_clearColor), color);
        }

        ID3D12Resource* GetResource() const noexcept { return m_resource.Get(); }
        D3D12_RESOURCE_STATES GetState() const noexcept { return m_state; }
        UINT GetSrvDescriptorHeapIndex() const noexcept { return m_srvDescriptorHeapIndex; }
        UINT GetUavDescriptorHeapIndex() const noexcept { return m_uavDescriptorHeapIndex; }
        UINT GetRtvDescriptorHeapIndex() const noexcept { return m_rtvDescriptorHeapIndex; }

        // Use when a state transition was applied to the resource directly
        void UpdateState(D3D12_RESOURCE_STATES state) noexcept { m_state = state; }

        DXGI_FORMAT GetFormat() const noexcept { return m_format; }

    private:
        ID3D12Device*                                       m_device;
        Microsoft::WRL::ComPtr<ID3D12Resource>              m_resource;
        D3D12_RESOURCE_STATES                               m_state;
        D3D12_CPU_DESCRIPTOR_HANDLE                         m_srvDescriptor;
        D3D12_CPU_DESCRIPTOR_HANDLE                         m_uavDescriptor;
        D3D12_CPU_DESCRIPTOR_HANDLE                         m_rtvDescriptor;
        UINT                                                m_srvDescriptorHeapIndex;
        UINT                                                m_uavDescriptorHeapIndex;
        UINT                                                m_rtvDescriptorHeapIndex;
        float                                               m_clearColor[4];

        DXGI_FORMAT                                         m_format;

        UINT                                                m_width;
        UINT                                                m_height;
    };
}
