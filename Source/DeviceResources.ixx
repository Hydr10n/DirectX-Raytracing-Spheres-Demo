module;

#include <memory>
#include <stdexcept>

#include <wrl.h>

#include "directx/d3d12.h"

#include "D3D12MemAlloc.h"

export module DeviceResources;

import CommandList;
import DescriptorHeap;
import DeviceContext;
import ErrorHelpers;
import Texture;

using namespace D3D12MA;
using namespace DirectX;
using namespace ErrorHelpers;
using namespace Microsoft::WRL;
using namespace Microsoft::WRL::Wrappers;
using namespace std;

export namespace DX
{
	class IDeviceNotify
	{
	public:
		virtual void OnDeviceLost() = 0;
		virtual void OnDeviceRestored() = 0;

	protected:
		~IDeviceNotify() = default;
	};

	class DeviceResources
	{
	public:
		static constexpr uint32_t MinBackBufferCount = 2, MaxBackBufferCount = 3;

		struct OptionFlags
		{
			enum
			{
				DisableGPUTimeout = 0x1,
				ReverseDepth = 0x2
			};
		};

		struct CreationDesc {
			D3D_FEATURE_LEVEL MinFeatureLevel = D3D_FEATURE_LEVEL_12_0;
			D3D12_RAYTRACING_TIER MinRaytracingTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
			uint32_t
				DefaultDescriptorHeapCapacity = 1 << 8,
				ResourceDescriptorHeapCapacity = 1 << 16,
				RenderDescriptorHeapCapacity = 1 << 8,
				DepthStencilDescriptorHeapCapacity = 1 << 8;
			DXGI_FORMAT BackBufferFormat = DXGI_FORMAT_B8G8R8A8_UNORM, DepthStencilBufferFormat = DXGI_FORMAT_D32_FLOAT;
			uint32_t BackBufferCount = MinBackBufferCount, OptionFlags = 0;
		};

		DeviceResources(const DeviceResources&) = delete;
		DeviceResources& operator=(const DeviceResources&) = delete;

		explicit DeviceResources(const CreationDesc& creationDesc = {}) noexcept(false) : m_creationDesc(creationDesc)
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

		~DeviceResources() { WaitForGPU(); }

		void RegisterDeviceNotify(IDeviceNotify* pDeviceNotify) noexcept { m_deviceNotify = pDeviceNotify; }

		void CreateDeviceResources();

		void CreateWindowSizeDependentResources();

		void UpdateColorSpace();

		bool ResizeWindow(SIZE size)
		{
			if (size.cx == m_outputSize.cx && size.cy == m_outputSize.cy)
			{
				UpdateColorSpace();

				return false;
			}

			m_outputSize = size;
			CreateWindowSizeDependentResources();

			return true;
		}

		void SetWindow(HWND window, SIZE size)
		{
			m_window = window;
			ResizeWindow(size);
		}

		bool EnableVSync(bool value) noexcept
		{
			const bool ret = value || m_isTearingSupported;
			if (ret)
			{
				m_isVSyncEnabled = value;
			}
			return ret;
		}

		void RequestHDR(bool value) noexcept
		{
			m_isHDRRequested = value;

			UpdateColorSpace();
		}

		void Prepare();
		void Present();

		void WaitForGPU() noexcept
		{
			if (m_commandList)
			{
				try
				{
					m_commandList->Wait();
				}
				catch (...) {}
			}
		}

		const CreationDesc& GetCreationDesc() const noexcept { return m_creationDesc; }

		const DeviceContext& GetDeviceContext() const noexcept { return *m_deviceContext; }

		auto GetDXGIFactory() const noexcept { return m_dxgiFactory.Get(); }

		D3D_FEATURE_LEVEL GetFeatureLevel() const noexcept { return m_featureLevel; }
		D3D12_RAYTRACING_TIER GetRaytracingTier() const noexcept { return m_raytracingTier; }
		auto GetAdapter() const noexcept { return m_adapter.Get(); }

		const auto& GetCommandList() const noexcept { return *m_commandList; }
		auto& GetCommandList() noexcept { return *m_commandList; }

		HWND GetWindow() const noexcept { return m_window; }
		SIZE GetOutputSize() const noexcept { return m_outputSize; }
		const D3D12_VIEWPORT& GetScreenViewport() const noexcept { return m_screenViewport; }
		const D3D12_RECT& GetScissorRect() const noexcept { return m_scissorRect; }

		bool IsTearingSupported() const noexcept { return m_isTearingSupported; }
		bool IsVSyncEnabled() const noexcept { return m_isVSyncEnabled; }
		bool IsHDRSupported() const noexcept { return m_isHDRSupported; }
		bool IsHDREnabled() const noexcept { return m_isHDRSupported && m_isHDRRequested; }
		DXGI_COLOR_SPACE_TYPE GetColorSpace() const noexcept { return m_colorSpace; }
		auto GetSwapChain() const noexcept { return m_swapChain.Get(); }

		uint32_t GetCurrentBackBufferIndex() const noexcept { return m_backBufferIndex; }
		const Texture& GetBackBuffer() const noexcept { return *m_backBuffers[m_backBufferIndex]; }
		Texture& GetBackBuffer() noexcept { return *m_backBuffers[m_backBufferIndex]; }

		const Texture& GetDepthStencilBuffer() const noexcept { return *m_depthStencilBuffer; }
		Texture& GetDepthStencilBuffer() noexcept { return *m_depthStencilBuffer; }

	private:
		const CreationDesc m_creationDesc;

		IDeviceNotify* m_deviceNotify{};

		unique_ptr<DeviceContext> m_deviceContext;

		DWORD m_dxgiFactoryFlags{};
		ComPtr<IDXGIFactory4> m_dxgiFactory;

		D3D_FEATURE_LEVEL m_featureLevel = D3D_FEATURE_LEVEL_12_0;
		D3D12_RAYTRACING_TIER m_raytracingTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
		ComPtr<IDXGIAdapter1> m_adapter;
		ComPtr<ID3D12Device5> m_device;

		ComPtr<Allocator> m_memoryAllocator;

		ComPtr<ID3D12CommandQueue> m_commandQueue;
		unique_ptr<CommandList> m_commandList;

		UINT64 m_fenceValues[MaxBackBufferCount]{};
		ComPtr<ID3D12Fence> m_fence;
		Event m_fenceEvent;

		unique_ptr<DescriptorHeapEx>
			m_defaultDescriptorHeap,
			m_resourceDescriptorHeap,
			m_renderDescriptorHeap,
			m_depthStencilDescriptorHeap;

		HWND m_window{};
		SIZE m_outputSize{};
		D3D12_VIEWPORT m_screenViewport{};
		D3D12_RECT m_scissorRect{};

		bool m_isTearingSupported{}, m_isVSyncEnabled = true, m_isHDRSupported{}, m_isHDRRequested{};
		DXGI_COLOR_SPACE_TYPE m_colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
		ComPtr<IDXGISwapChain3> m_swapChain;

		uint32_t m_backBufferIndex{};
		unique_ptr<Texture> m_backBuffers[MaxBackBufferCount], m_depthStencilBuffer;

		void CreateDevice();

		void MoveToNextFrame();

		void OnDeviceLost();
	};
}
