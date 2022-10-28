//--------------------------------------------------------------------------------------
// File: RenderTexture.cpp
//
// Helper for managing offscreen render targets
//
// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
//-------------------------------------------------------------------------------------

#include "pch.h"
#include "RenderTexture.h"

#include "directxtk12/DirectXHelpers.h"

#include <algorithm>
#include <cstdio>
#include <stdexcept>

using namespace DirectX;
using namespace DX;

using Microsoft::WRL::ComPtr;

#ifdef __MINGW32__
#define DX_CONSTEXPR const
#else
#define DX_CONSTEXPR constexpr
#endif

RenderTexture::RenderTexture(DXGI_FORMAT format) noexcept :
    m_state(D3D12_RESOURCE_STATE_COMMON),
    m_srvDescriptor{},
    m_rtvDescriptor{},
    m_srvDescriptorHeapIndex{},
    m_uavDescriptorHeapIndex{},
    m_rtvDescriptorHeapIndex{},
    m_clearColor{},
    m_format(format),
    m_width(0),
    m_height(0)
{
}

void RenderTexture::SetDevice(_In_ ID3D12Device* device,
    DescriptorHeap* resourceDescriptorHeap, UINT srvDescriptorHeapIndex, UINT uavDescriptorHeapIndex,
    DescriptorHeap* renderDescriptorHeap, UINT rtvDescriptorHeapIndex)
{
    {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT formatSupport = { m_format, D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE };
        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &formatSupport, sizeof(formatSupport))))
        {
            throw std::runtime_error("CheckFeatureSupport");
        }

        DX_CONSTEXPR UINT required = D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_RENDER_TARGET;
        if ((formatSupport.Support1 & required) != required)
        {
#ifdef _DEBUG
            char buff[128] = {};
            sprintf_s(buff, "RenderTexture: Device does not support the requested format (%u)!\n", m_format);
            OutputDebugStringA(buff);
#endif
            throw std::runtime_error("RenderTexture");
        }
    }

    m_device = device;

    if (srvDescriptorHeapIndex != ~0u)
    {
        if (resourceDescriptorHeap == nullptr)
        {
            throw std::runtime_error("RenderTexture");
        }

        m_srvDescriptor = resourceDescriptorHeap->GetCpuHandle(srvDescriptorHeapIndex);
    }

    if (uavDescriptorHeapIndex != ~0u)
    {
        if (resourceDescriptorHeap == nullptr)
        {
            throw std::runtime_error("RenderTexture");
        }

        m_uavDescriptor = resourceDescriptorHeap->GetCpuHandle(uavDescriptorHeapIndex);
    }

    if (rtvDescriptorHeapIndex != ~0u)
    {
        if (renderDescriptorHeap == nullptr)
        {
            throw std::runtime_error("RenderTexture");
        }

        m_rtvDescriptor = renderDescriptorHeap->GetCpuHandle(rtvDescriptorHeapIndex);
    }

    m_srvDescriptorHeapIndex = srvDescriptorHeapIndex;
    m_uavDescriptorHeapIndex = uavDescriptorHeapIndex;
    m_rtvDescriptorHeapIndex = rtvDescriptorHeapIndex;
}

void RenderTexture::CreateResource(UINT64 width, UINT height)
{
    if (width == m_width && height == m_height)
        return;

    if (m_width > UINT32_MAX || m_height > UINT32_MAX)
    {
        throw std::out_of_range("Invalid width/height");
    }

    if (!m_device)
        return;

    width = std::max<UINT64>(width, 1);
    height = std::max<UINT>(height, 1);

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
            IID_GRAPHICS_PPV_ARGS(m_resource.ReleaseAndGetAddressOf()))
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

void RenderTexture::TransitionTo(_In_ ID3D12GraphicsCommandList* commandList,
    D3D12_RESOURCE_STATES afterState)
{
    TransitionResource(commandList, m_resource.Get(), m_state, afterState);
    m_state = afterState;
}
