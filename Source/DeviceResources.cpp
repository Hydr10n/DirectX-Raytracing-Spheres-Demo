module;

#include <format>

#include <dxgi1_6.h>
#include "directx/d3dx12.h"

#ifdef _DEBUG
#include <dxgidebug.h>
#endif

#include "directxtk12/SimpleMath.h"

#include "D3D12MemAlloc.h"

#include "rtxmu/D3D12AccelStructManager.h"

module DeviceResources;

import DescriptorHeap;
import ErrorHelpers;
import Texture;

using namespace DirectX;
using namespace DirectX::SimpleMath;
using namespace DX;
using namespace ErrorHelpers;
using namespace Microsoft::WRL;
using namespace std;

DeviceResources::DeviceResources(const CreationDesc& creationDesc) noexcept(false) : m_creationDesc(creationDesc)
{
	if (creationDesc.BackBufferCount < MinBackBufferCount || creationDesc.BackBufferCount > MaxBackBufferCount)
	{
		Throw<out_of_range>("Invalid back buffer count");
	}

	if (creationDesc.MinFeatureLevel < D3D_FEATURE_LEVEL_12_0)
	{
		Throw<out_of_range>("Min feature level too low");
	}
}

DeviceResources::~DeviceResources() { WaitForGPU(); }

void DeviceResources::CreateDeviceResources()
{
#ifdef _DEBUG
	// Enable the debug layer (requires the Graphics Tools "optional feature").
	// NOTE: Enabling the debug layer after device creation will invalidate the active device.
	{
		if (ComPtr<ID3D12Debug> debug; SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
		{
			debug->EnableDebugLayer();
		}
		else
		{
			OutputDebugStringW(L"WARNING: Direct3D debug layer is not available\n");
		}

		if (ComPtr<IDXGIInfoQueue> infoQueue; SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&infoQueue))))
		{
			m_dxgiFactoryFlags = DXGI_CREATE_FACTORY_DEBUG;

			infoQueue->SetBreakOnSeverity(DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_CORRUPTION, true);
			infoQueue->SetBreakOnSeverity(DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_ERROR, true);

			DXGI_INFO_QUEUE_MESSAGE_ID IDs[]
			{
				80 // IDXGISwapChain::GetContainingOutput: The swapchain's adapter does not control the output on which the swapchain's window resides.
			};
			DXGI_INFO_QUEUE_FILTER filter
			{
				.DenyList
				{
					.NumIDs = static_cast<UINT>(size(IDs)),
					.pIDList = IDs
				}
			};
			infoQueue->AddStorageFilterEntries(DXGI_DEBUG_DXGI, &filter);
		}
	}
#endif

	ThrowIfFailed(CreateDXGIFactory2(m_dxgiFactoryFlags, IID_PPV_ARGS(&m_dxgiFactory)));

	CreateDevice();

#ifdef _DEBUG
	if (ComPtr<ID3D12InfoQueue> infoQueue; SUCCEEDED(m_device.As(&infoQueue)))
	{
		infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, true);
		infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, true);

		D3D12_MESSAGE_ID IDs[]
		{
			D3D12_MESSAGE_ID_MAP_INVALID_NULLRANGE,
			D3D12_MESSAGE_ID_UNMAP_INVALID_NULLRANGE,
			D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
			// Workarounds for debug layer issues on hybrid-graphics systems
			D3D12_MESSAGE_ID_EXECUTECOMMANDLISTS_WRONGSWAPCHAINBUFFERREFERENCE,
			D3D12_MESSAGE_ID_RESOURCE_BARRIER_MISMATCHING_COMMAND_LIST_TYPE
		};
		D3D12_INFO_QUEUE_FILTER filter
		{
			.DenyList
			{
				.NumIDs = static_cast<UINT>(size(IDs)),
				.pIDList = IDs
			}
		};
		infoQueue->AddStorageFilterEntries(&filter);
	}
#endif

	constexpr D3D_FEATURE_LEVEL featureLevels[]
	{
#if defined(NTDDI_WIN10_FE) || defined(USING_D3D12_AGILITY_SDK)
		D3D_FEATURE_LEVEL_12_2,
#endif
		D3D_FEATURE_LEVEL_12_1,
		D3D_FEATURE_LEVEL_12_0
	};
	D3D12_FEATURE_DATA_FEATURE_LEVELS featureData{ static_cast<UINT>(size(featureLevels)), featureLevels, D3D_FEATURE_LEVEL_12_0 };
	if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &featureData, sizeof(featureData))))
	{
		m_featureLevel = featureData.MaxSupportedFeatureLevel;
	}
	else
	{
		m_featureLevel = m_creationDesc.MinFeatureLevel;
	}

	const D3D12MA::ALLOCATOR_DESC desc{ .pDevice = m_device.Get(), .pAdapter = GetAdapter() };
	ThrowIfFailed(D3D12MA::CreateAllocator(&desc, &m_memoryAllocator));

	if (m_raytracingTier != D3D12_RAYTRACING_TIER_NOT_SUPPORTED)
	{
		m_accelerationStructureManager = make_unique<rtxmu::DxAccelStructManager>(
			m_device.Get()
#ifdef _DEBUG
			, Logger::Level::DBG
#endif
			);
		m_accelerationStructureManager->Initialize();
	}

	const D3D12_COMMAND_QUEUE_DESC commandQueueDesc
	{
		.Type = D3D12_COMMAND_LIST_TYPE_DIRECT,
		.Flags = m_creationDesc.OptionFlags & OptionFlags::DisableGPUTimeout ? D3D12_COMMAND_QUEUE_FLAG_DISABLE_GPU_TIMEOUT : D3D12_COMMAND_QUEUE_FLAG_NONE
	};
	ThrowIfFailed(m_device->CreateCommandQueue(&commandQueueDesc, IID_PPV_ARGS(&m_commandQueue)));

	m_defaultDescriptorHeap = make_unique<DescriptorHeapEx>(m_device.Get(), m_creationDesc.DefaultDescriptorHeapCapacity, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, D3D12_DESCRIPTOR_HEAP_FLAG_NONE);
	m_resourceDescriptorHeap = make_unique<DescriptorHeapEx>(m_device.Get(), m_creationDesc.ResourceDescriptorHeapCapacity);
	m_renderDescriptorHeap = make_unique<DescriptorHeapEx>(m_device.Get(), m_creationDesc.RenderDescriptorHeapCapacity, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, D3D12_DESCRIPTOR_HEAP_FLAG_NONE);
	m_depthStencilDescriptorHeap = make_unique<DescriptorHeapEx>(m_device.Get(), m_creationDesc.DepthStencilDescriptorHeapCapacity, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, D3D12_DESCRIPTOR_HEAP_FLAG_NONE);

	m_deviceContext = make_unique<DeviceContext>(
		m_device.Get(),
		m_commandQueue.Get(),
		m_memoryAllocator.Get(),
		m_accelerationStructureManager.get(),
		m_defaultDescriptorHeap.get(),
		m_resourceDescriptorHeap.get(),
		m_renderDescriptorHeap.get(),
		m_depthStencilDescriptorHeap.get()
		);

	m_commandList = make_unique<CommandList>(*m_deviceContext);

	ThrowIfFailed(m_device->CreateFence(m_fenceValues[m_backBufferIndex]++, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
	m_fenceEvent.Attach(CreateEvent(nullptr, FALSE, FALSE, nullptr));
	ThrowIfFailed(static_cast<BOOL>(m_fenceEvent.IsValid()));

	{
		BOOL allowTearing;

		ComPtr<IDXGIFactory5> factory5;
		HRESULT hr = m_dxgiFactory.As(&factory5);
		if (SUCCEEDED(hr))
		{
			hr = factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing));
		}

		if (FAILED(hr) || !allowTearing)
		{
#ifdef _DEBUG
			OutputDebugStringW(L"WARNING: Variable refresh rate displays not supported\n");
#endif
		}

		m_isTearingSupported = allowTearing;
	}
}

void DeviceResources::CreateWindowSizeDependentResources()
{
	if (m_window == nullptr)
	{
		Throw<logic_error>("Valid HWND required");
	}

	WaitForGPU();

	for (uint32_t i = 0; i < m_creationDesc.BackBufferCount; i++)
	{
		m_backBuffers[i].reset();
		m_fenceValues[i] = m_fenceValues[m_backBufferIndex];
	}

	constexpr auto NoSRGB = [](DXGI_FORMAT format)
	{
		switch (format)
		{
			case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
			case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
			case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_UNORM;
			default: return format;
		}
	};
	if (const DXGI_FORMAT backBufferFormat = NoSRGB(m_creationDesc.BackBufferFormat);
		m_swapChain)
	{
		HRESULT hr = m_swapChain->ResizeBuffers(
			m_creationDesc.BackBufferCount,
			m_outputSize.cx, m_outputSize.cy,
			backBufferFormat,
			m_isTearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u
		);

		if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
		{
#ifdef _DEBUG
			OutputDebugStringW(format(
				L"Device Lost on Present: Reason code 0x{:08X}\n",
				static_cast<UINT>(hr == DXGI_ERROR_DEVICE_REMOVED ? m_device->GetDeviceRemovedReason() : hr)
			).c_str());
#endif
			OnDeviceLost();

			return;
		}

		ThrowIfFailed(hr);
	}
	else
	{
		const DXGI_SWAP_CHAIN_DESC1 swapChainDesc
		{
			.Width = static_cast<UINT>(m_outputSize.cx),
			.Height = static_cast<UINT>(m_outputSize.cy),
			.Format = backBufferFormat,
			.SampleDesc{
				.Count = 1
			},
			.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
			.BufferCount = m_creationDesc.BackBufferCount,
			.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
			.AlphaMode = DXGI_ALPHA_MODE_IGNORE,
			.Flags = m_isTearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u
		};
		constexpr DXGI_SWAP_CHAIN_FULLSCREEN_DESC swapChainFullscreen{ .Windowed = TRUE };
		ComPtr<IDXGISwapChain1> swapChain;
		ThrowIfFailed(m_dxgiFactory->CreateSwapChainForHwnd(
			m_commandQueue.Get(),
			m_window,
			&swapChainDesc, &swapChainFullscreen,
			nullptr, &swapChain
		));
		ThrowIfFailed(swapChain.As(&m_swapChain));

		// This class does not support exclusive full-screen mode and prevents DXGI from responding to the ALT+ENTER shortcut
		ThrowIfFailed(m_dxgiFactory->MakeWindowAssociation(m_window, DXGI_MWA_NO_ALT_ENTER));
	}

	UpdateColorSpace();

	for (uint32_t i = 0; i < m_creationDesc.BackBufferCount; i++)
	{
		ComPtr<ID3D12Resource> buffer;
		ThrowIfFailed(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&buffer)));

		m_backBuffers[i] = make_unique<Texture>(*m_deviceContext, buffer.Get(), D3D12_RESOURCE_STATE_PRESENT, false);
		m_backBuffers[i]->CreateRTV();
	}

	m_backBufferIndex = m_swapChain->GetCurrentBackBufferIndex();

	if (m_creationDesc.DepthStencilBufferFormat != DXGI_FORMAT_UNKNOWN)
	{
		m_depthStencilBuffer = make_unique<Texture>(
			*m_deviceContext,
			Texture::CreationDesc{
				.Format = m_creationDesc.DepthStencilBufferFormat,
				.Width = static_cast<UINT>(m_outputSize.cx),
				.Height = static_cast<UINT>(m_outputSize.cy),
				.ClearColor = Color(CD3DX12_CLEAR_VALUE(
					m_creationDesc.DepthStencilBufferFormat,
					m_creationDesc.OptionFlags & OptionFlags::ReverseDepth ? D3D12_MIN_DEPTH : D3D12_MAX_DEPTH, 0
				).Color)
			}.AsDepthStencil()
			);
		m_depthStencilBuffer->CreateDSV();
	}

	m_screenViewport =
	{
		.Width = static_cast<float>(m_outputSize.cx),
		.Height = static_cast<float>(m_outputSize.cy),
		.MinDepth = D3D12_MIN_DEPTH,
		.MaxDepth = D3D12_MAX_DEPTH
	};
	m_scissorRect = { .right = m_outputSize.cx, .bottom = m_outputSize.cy };
}

void DeviceResources::UpdateColorSpace()
{
	if (!m_dxgiFactory)
	{
		return;
	}

	if (!m_dxgiFactory->IsCurrent())
	{
		ThrowIfFailed(CreateDXGIFactory2(m_dxgiFactoryFlags, IID_PPV_ARGS(&m_dxgiFactory)));
	}

	bool isDisplayHDR10 = false;

	if (m_swapChain)
	{
		// To detect HDR support, we will need to check the color space in the primary
		// DXGI output associated with the app at this point in time
		// (using window/display intersection).

		RECT windowRect;
		ThrowIfFailed(GetWindowRect(m_window, &windowRect));

		ComPtr<IDXGIOutput> bestOutput;
		long bestIntersectArea = -1;

		ComPtr<IDXGIAdapter> adapter;
		for (uint32_t adapterIndex = 0; SUCCEEDED(m_dxgiFactory->EnumAdapters(adapterIndex, &adapter)); ++adapterIndex)
		{
			ComPtr<IDXGIOutput> output;
			for (uint32_t outputIndex = 0; SUCCEEDED(adapter->EnumOutputs(outputIndex, &output)); ++outputIndex)
			{
				DXGI_OUTPUT_DESC outputDesc;
				ThrowIfFailed(output->GetDesc(&outputDesc));

				constexpr auto ComputeIntersectionArea = [](const RECT& a, const RECT& b)
				{
					return max(0l, min(a.right, b.right) - max(a.left, b.left)) * max(0l, min(a.bottom, b.bottom) - max(a.top, b.top));
				};
				if (const long intersectArea = ComputeIntersectionArea(windowRect, outputDesc.DesktopCoordinates);
					intersectArea > bestIntersectArea)
				{
					bestOutput.Swap(output);
					bestIntersectArea = intersectArea;
				}
			}
		}

		if (bestOutput)
		{
			if (ComPtr<IDXGIOutput6> output; SUCCEEDED(bestOutput.As(&output)))
			{
				DXGI_OUTPUT_DESC1 desc;
				ThrowIfFailed(output->GetDesc1(&desc));

				if (desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)
				{
					isDisplayHDR10 = true;
				}
			}
		}
	}

	DXGI_COLOR_SPACE_TYPE colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
	if (m_isHDRRequested && isDisplayHDR10)
	{
		switch (m_creationDesc.BackBufferFormat)
		{
			case DXGI_FORMAT_R10G10B10A2_UNORM:
				// The application creates the HDR10 signal.
				colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
				break;

			case DXGI_FORMAT_R16G16B16A16_FLOAT:
				// The system creates the HDR10 signal; application uses linear values.
				colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
				break;

			default: break;
		}
	}

	if (uint32_t colorSpaceSupport;
		m_swapChain && SUCCEEDED(m_swapChain->CheckColorSpaceSupport(colorSpace, &colorSpaceSupport))
		&& (colorSpaceSupport & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT))
	{
		ThrowIfFailed(m_swapChain->SetColorSpace1(colorSpace));
		m_colorSpace = colorSpace;
		m_isHDRSupported = isDisplayHDR10;
	}
	else
	{
		m_isHDRSupported = false;
	}
}

void DeviceResources::Prepare()
{
	m_commandList->Begin();

	m_commandList->SetRenderTarget(GetBackBuffer());
}

void DeviceResources::Present()
{
	m_commandList->SetState(GetBackBuffer(), D3D12_RESOURCE_STATE_PRESENT);

	m_commandList->End(false);

	HRESULT hr;
	if (m_isVSyncEnabled)
	{
		// The first argument instructs DXGI to block until VSync, putting the application
		// to sleep until the next VSync. This ensures we don't waste any cycles rendering
		// frames that will never be displayed to the screen.
		hr = m_swapChain->Present(1, 0);
	}
	else
	{
		// Recommended to always use tearing if supported when using a sync interval of 0.
		// Note this will fail if in true 'fullscreen' mode.
		hr = m_swapChain->Present(0, DXGI_PRESENT_ALLOW_TEARING);
	}

	if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
	{
#ifdef _DEBUG
		OutputDebugStringW(format(
			L"Device Lost on Present: Reason code 0x{:08X}\n",
			static_cast<UINT>(hr == DXGI_ERROR_DEVICE_REMOVED ? m_device->GetDeviceRemovedReason() : hr)
		).c_str());
#endif
		OnDeviceLost();
	}
	else
	{
		ThrowIfFailed(hr);

		MoveToNextFrame();

		if (!m_dxgiFactory->IsCurrent())
		{
			UpdateColorSpace();
		}
	}
}

void DeviceResources::CreateDevice()
{
	ComPtr<IDXGIAdapter1> adapter;
	ComPtr<ID3D12Device5> device;
	D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};

	const auto CreateRaytracingDevice = [&](uint32_t adapterIndex)
	{
		DXGI_ADAPTER_DESC1 desc;
		ThrowIfFailed(adapter->GetDesc1(&desc));

		if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
		{
			ThrowIfFailed(D3D12CreateDevice(adapter.Get(), m_creationDesc.MinFeatureLevel, IID_PPV_ARGS(&device)));

			if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5)))
				&& options5.RaytracingTier >= m_creationDesc.MinRaytracingTier)
			{
#ifdef _DEBUG
				OutputDebugStringW(format(
					L"Direct3D Adapter {}: VID:{:04X}, PID:{:04X} - {}\n",
					adapterIndex, desc.VendorId, desc.DeviceId, desc.Description
				).c_str());
#endif

				return true;
			}
		}

		return false;
	};

	if (ComPtr<IDXGIFactory6> factory; SUCCEEDED(m_dxgiFactory.As(&factory)))
	{
		for (uint32_t i = 0;
			SUCCEEDED(factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)));
			i++)
		{
			if (CreateRaytracingDevice(i))
			{
				break;
			}
		}
	}

	if (options5.RaytracingTier < m_creationDesc.MinRaytracingTier)
	{
		throw runtime_error(format(
			"DirectX Raytracing Tier {}.{} is not supported on this device.",
			m_creationDesc.MinRaytracingTier / 10, m_creationDesc.MinRaytracingTier % 10
		));
	}

	if (!adapter)
	{
		throw runtime_error("No Direct3D 12 device found");
	}

	m_raytracingTier = options5.RaytracingTier;
	m_device = device;
	m_adapter = adapter;
}

void DeviceResources::MoveToNextFrame()
{
	const UINT64 fenceValue = m_fenceValues[m_backBufferIndex];

	ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), fenceValue));

	m_backBufferIndex = m_swapChain->GetCurrentBackBufferIndex();

	if (m_fence->GetCompletedValue() < m_fenceValues[m_backBufferIndex])
	{
		ThrowIfFailed(m_fence->SetEventOnCompletion(m_fenceValues[m_backBufferIndex], m_fenceEvent.Get()));
		ignore = WaitForSingleObject(m_fenceEvent.Get(), INFINITE);
	}

	m_fenceValues[m_backBufferIndex] = fenceValue + 1;
}

void DeviceResources::OnDeviceLost()
{
	if (m_deviceNotify)
	{
		m_deviceNotify->OnDeviceLost();
	}

	m_depthStencilBuffer.reset();
	for (uint32_t i = 0; i < m_creationDesc.BackBufferCount; i++)
	{
		m_backBuffers[i].reset();
	}

	m_swapChain.Reset();

	m_depthStencilDescriptorHeap.reset();
	m_renderDescriptorHeap.reset();
	m_resourceDescriptorHeap.reset();
	m_defaultDescriptorHeap.reset();

	m_fence.Reset();

	m_commandList.reset();
	m_commandQueue.Reset();

	m_accelerationStructureManager.reset();

	m_memoryAllocator.Reset();

	m_device.Reset();
	m_adapter.Reset();

	m_dxgiFactory.Reset();

#ifdef _DEBUG
	{
		if (ComPtr<IDXGIDebug1> dxgiDebug; SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&dxgiDebug))))
		{
			dxgiDebug->ReportLiveObjects(DXGI_DEBUG_ALL, static_cast<DXGI_DEBUG_RLO_FLAGS>(DXGI_DEBUG_RLO_SUMMARY | DXGI_DEBUG_RLO_IGNORE_INTERNAL));
		}
	}
#endif

	CreateDeviceResources();
	CreateWindowSizeDependentResources();

	if (m_deviceNotify)
	{
		m_deviceNotify->OnDeviceRestored();
	}
}
