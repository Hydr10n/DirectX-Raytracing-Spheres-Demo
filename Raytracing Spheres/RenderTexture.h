//--------------------------------------------------------------------------------------
// File: RenderTexture.h
//
// Helper for managing offscreen render targets
//
// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
//-------------------------------------------------------------------------------------

#pragma once

#include <wrl/client.h>

#include <d3d12.h>

#include <DirectXMath.h>

#include "directxtk12/DescriptorHeap.h"

namespace DX
{
    class RenderTexture
    {
    public:
        explicit RenderTexture(DXGI_FORMAT format) noexcept;

        RenderTexture(RenderTexture&&) = default;
        RenderTexture& operator= (RenderTexture&&) = default;

        RenderTexture(RenderTexture const&) = delete;
        RenderTexture& operator= (RenderTexture const&) = delete;

        void SetDevice(_In_ ID3D12Device* device,
            const DirectX::DescriptorHeap* resourceDescriptorHeap = nullptr, UINT srvDescriptorHeapIndex = ~0u, UINT uavDescriptorHeapIndex = ~0u,
            const DirectX::DescriptorHeap* renderDescriptorHeap = nullptr, UINT rtvDescriptorHeapIndex = ~0u);

        void CreateResource(UINT64 width, UINT height);

        void TransitionTo(_In_ ID3D12GraphicsCommandList* commandList, D3D12_RESOURCE_STATES afterState);

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

        void SetClearColor(DirectX::FXMVECTOR color)
        {
            DirectX::XMStoreFloat4(reinterpret_cast<DirectX::XMFLOAT4*>(m_clearColor), color);
        }

        ID3D12Resource* GetResource() const noexcept { return m_resource.Get(); }
        D3D12_RESOURCE_STATES GetCurrentState() const noexcept { return m_state; }
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

        UINT64                                              m_width;
        UINT                                              m_height;
    };
}
