module;

#include <filesystem>

#include "directx/d3dx12.h"

#include "directxtk12/DirectXHelpers.h"
#include "directxtk12/SimpleMath.h"

#include "D3D12MemAlloc.h"

export module Texture;

import DescriptorHeap;
import ErrorHelpers;
import GPUMemoryAllocator;
import GPUResource;

using namespace D3D12MA;
using namespace DirectX;
using namespace DirectX::SimpleMath;
using namespace ErrorHelpers;
using namespace Microsoft::WRL;
using namespace std;

export namespace DirectX {
	enum class TextureMap {
		Unknown,
		BaseColor,
		EmissiveColor,
		Metallic,
		Roughness,
		AmbientOcclusion,
		Transmission,
		Opacity,
		Normal,
		Cube
	};

	class Texture : public GPUResource {
	public:
		Texture(ID3D12Resource* pResource, D3D12_RESOURCE_STATES state) : GPUResource(pResource, state) {
			switch (pResource->GetDesc().Dimension) {
				case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
				case D3D12_RESOURCE_DIMENSION_TEXTURE2D:
				case D3D12_RESOURCE_DIMENSION_TEXTURE3D: break;
				default: throw invalid_argument("Resource is not texture");
			}

			const auto mipLevels = pResource->GetDesc().MipLevels;
			m_descriptors.UAV.resize(mipLevels);
			m_descriptors.RTV.resize(mipLevels);
		}

		Texture(ID3D12Device* pDevice, DXGI_FORMAT format, XMUINT2 size, UINT16 mipLevels = 1, const Color& clearColor = {}, D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_RENDER_TARGET) noexcept(false) :
			GPUResource(initialState), m_clearColor(clearColor) {
			constexpr ALLOCATION_DESC allocationDesc{ .HeapType = D3D12_HEAP_TYPE_DEFAULT };
			const auto resourceDesc = CD3DX12_RESOURCE_DESC::Tex2D(format, size.x, size.y, 1, mipLevels, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
			D3D12_CLEAR_VALUE clearValue{ format };
			reinterpret_cast<Color&>(clearValue.Color) = m_clearColor;
			ThrowIfFailed(GPUMemoryAllocator::Get(pDevice)->CreateResource(&allocationDesc, &resourceDesc, initialState, nullptr, &m_allocation, IID_PPV_ARGS(&m_resource)));

			m_descriptors.UAV.resize(mipLevels);
			m_descriptors.RTV.resize(mipLevels);
		}

		const Color& GetClearColor() const noexcept { return m_clearColor; }
		void SetClearColor(const Color& color) noexcept { m_clearColor = color; }
		void Clear(ID3D12GraphicsCommandList* pCommandList, UINT16 mipLevel = 0) { pCommandList->ClearRenderTargetView(m_descriptors.RTV[mipLevel].CPUHandle, reinterpret_cast<const float*>(&m_clearColor), 0, nullptr); }

		const Descriptor& GetSRVDescriptor() const noexcept { return m_descriptors.SRV; }
		Descriptor& GetSRVDescriptor() noexcept { return m_descriptors.SRV; }

		const Descriptor& GetUAVDescriptor(UINT16 mipLevel = 0) const noexcept { return m_descriptors.UAV[mipLevel]; }
		Descriptor& GetUAVDescriptor(UINT16 mipLevel = 0) noexcept { return m_descriptors.UAV[mipLevel]; }

		const Descriptor& GetRTVDescriptor(UINT16 mipLevel = 0) const noexcept { return m_descriptors.RTV[mipLevel]; }
		Descriptor& GetRTVDescriptor(UINT16 mipLevel = 0) noexcept { return m_descriptors.RTV[mipLevel]; }

		void CreateSRV(const DescriptorHeapEx& resourceDescriptorHeap, UINT index, bool isCubeMap = false) {
			m_descriptors.SRV = {
				.Index = index,
				.CPUHandle = resourceDescriptorHeap.GetCpuHandle(index),
				.GPUHandle = resourceDescriptorHeap.GetGpuHandle(index)
			};
			ComPtr<ID3D12Device> device;
			ThrowIfFailed((*this)->GetDevice(IID_PPV_ARGS(&device)));
			CreateShaderResourceView(device.Get(), *this, m_descriptors.SRV.CPUHandle, isCubeMap);
		}

		void CreateUAV(const DescriptorHeapEx& resourceDescriptorHeap, UINT index, UINT16 mipLevel = 0) {
			m_descriptors.UAV[mipLevel] = {
				.Index = index,
				.CPUHandle = resourceDescriptorHeap.GetCpuHandle(index),
				.GPUHandle = resourceDescriptorHeap.GetGpuHandle(index)
			};
			ComPtr<ID3D12Device> device;
			ThrowIfFailed((*this)->GetDevice(IID_PPV_ARGS(&device)));
			CreateUnorderedAccessView(device.Get(), *this, m_descriptors.UAV[mipLevel].CPUHandle, mipLevel);
		}

		void CreateRTV(const DescriptorHeapEx& renderDescriptorHeap, UINT index, UINT16 mipLevel = 0) {
			m_descriptors.RTV[mipLevel] = {
				.Index = index,
				.CPUHandle = renderDescriptorHeap.GetCpuHandle(index)
			};
			ComPtr<ID3D12Device> device;
			ThrowIfFailed((*this)->GetDevice(IID_PPV_ARGS(&device)));
			CreateRenderTargetView(device.Get(), *this, m_descriptors.RTV[mipLevel].CPUHandle, mipLevel);
		}

	private:
		Color m_clearColor;

		struct {
			Descriptor SRV;
			vector<Descriptor> UAV, RTV;
		} m_descriptors;
	};
}
