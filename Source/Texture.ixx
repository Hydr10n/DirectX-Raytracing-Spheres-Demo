module;

#include <filesystem>

#include "directx/d3dx12.h"

#include "directxtk12/DirectXHelpers.h"
#include "directxtk12/SimpleMath.h"

#include "D3D12MemAlloc.h"

export module Texture;

import DescriptorHeap;
import DeviceContext;
import ErrorHelpers;
import GPUResource;

using namespace D3D12MA;
using namespace DirectX;
using namespace DirectX::SimpleMath;
using namespace ErrorHelpers;
using namespace Microsoft::WRL;
using namespace std;

export namespace DirectX {
	enum class TextureMapType {
		BaseColor,
		EmissiveColor,
		Metallic,
		Roughness,
		AmbientOcclusion,
		Transmission,
		Opacity,
		Normal,
		Cube,
		_2DCount = Cube
	};

	enum class TextureDimension { _1, _2, _3 };

	class Texture : public GPUResource {
	public:
		struct CreationDesc {
			D3D12_HEAP_TYPE HeapType = D3D12_HEAP_TYPE_DEFAULT;
			DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
			TextureDimension Dimension = TextureDimension::_2;
			UINT Width{}, Height{};
			UINT16 DepthOrArraySize = 1, MipLevels = 1;
			UINT SampleCount = 1;
			Color ClearColor{};
			D3D12_RESOURCE_FLAGS Flags = D3D12_RESOURCE_FLAG_NONE;
			D3D12_RESOURCE_STATES InitialState = D3D12_RESOURCE_STATE_COMMON;
			bool KeepInitialState{};

			auto& AsRenderTarget() {
				Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
				InitialState = D3D12_RESOURCE_STATE_RENDER_TARGET;
				return *this;
			}

			auto& AsUnorderedAccess() {
				Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
				InitialState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
				return *this;
			}

			CreationDesc& AsDepthStencil() {
				Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
				InitialState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
				return *this;
			}
		};

		Texture(
			const DeviceContext& deviceContext,
			ID3D12Resource* pResource,
			D3D12_RESOURCE_STATES initialState, bool keepInitialState
		) : GPUResource(deviceContext, pResource, initialState, keepInitialState) {
			switch (pResource->GetDesc().Dimension) {
				case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
				case D3D12_RESOURCE_DIMENSION_TEXTURE2D:
				case D3D12_RESOURCE_DIMENSION_TEXTURE3D: break;
				default: Throw<invalid_argument>("Resource is not texture");
			}

			const auto desc = pResource->GetDesc();
			if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) m_descriptors.UAV.resize(desc.MipLevels);
			if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) m_descriptors.RTV.resize(desc.MipLevels);
			if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) m_descriptors.DSV.resize(desc.MipLevels);
		}

		Texture(const DeviceContext& deviceContext, const CreationDesc& creationDesc) noexcept(false) :
			GPUResource(deviceContext, creationDesc.InitialState, creationDesc.KeepInitialState),
			m_clearColor(creationDesc.ClearColor) {
			const auto& [HeapType, Format, Dimension, Width, Height, DepthOrArraySize, MipLevels, SampleCount, _, Flags, InitialState, _1] = creationDesc;
			const ALLOCATION_DESC allocationDesc{ .HeapType = HeapType };
			D3D12_RESOURCE_DESC resourceDesc;
			switch (Dimension)
			{
				case TextureDimension::_1:
					resourceDesc = CD3DX12_RESOURCE_DESC::Tex1D(Format, Width, DepthOrArraySize, MipLevels, Flags);
					break;

				case TextureDimension::_2:
					resourceDesc = CD3DX12_RESOURCE_DESC::Tex2D(Format, Width, Height, DepthOrArraySize, MipLevels, SampleCount, 0, Flags);
					break;

				case TextureDimension::_3:
					resourceDesc = CD3DX12_RESOURCE_DESC::Tex3D(Format, Width, Height, DepthOrArraySize, MipLevels, Flags);
					break;
			}
			ThrowIfFailed(deviceContext.MemoryAllocator->CreateResource(&allocationDesc, &resourceDesc, InitialState, nullptr, &m_allocation, IID_NULL, nullptr));

			if (Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) m_descriptors.UAV.resize(MipLevels);
			if (Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) m_descriptors.RTV.resize(MipLevels);
			if (Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) m_descriptors.DSV.resize(MipLevels);
		}

		const Color& GetClearColor() const noexcept { return m_clearColor; }
		void SetClearColor(const Color& color) noexcept { m_clearColor = color; }

		const auto& GetSRVDescriptor() const noexcept { return *m_descriptors.SRV; }
		const auto& GetUAVDescriptor(UINT16 mipLevel = 0) const noexcept { return *m_descriptors.UAV[mipLevel]; }
		const auto& GetRTVDescriptor(UINT16 mipLevel = 0) const noexcept { return *m_descriptors.RTV[mipLevel]; }
		const auto& GetDSVDescriptor(UINT16 mipLevel = 0) const noexcept { return *m_descriptors.DSV[mipLevel]; }

		void CreateSRV(bool isCubeMap = false) {
			auto& descriptor = m_descriptors.SRV;
			if (descriptor) return;
			descriptor = m_deviceContext.ResourceDescriptorHeap->Allocate();
			CreateShaderResourceView(m_deviceContext.Device, *this, *descriptor, isCubeMap);
		}

		void CreateUAV(UINT16 mipLevel = 0) {
			auto& descriptor = m_descriptors.UAV[mipLevel];
			if (descriptor) return;
			descriptor = m_deviceContext.ResourceDescriptorHeap->Allocate();
			CreateUnorderedAccessView(m_deviceContext.Device, *this, *descriptor, mipLevel);
		}

		void CreateRTV(UINT16 mipLevel = 0) {
			auto& descriptor = m_descriptors.RTV[mipLevel];
			if (descriptor) return;
			descriptor = m_deviceContext.RenderDescriptorHeap->Allocate();
			CreateRenderTargetView(m_deviceContext.Device, *this, *descriptor, mipLevel);
		}

		void CreateDSV(UINT16 mipLevel = 0) {
			auto& descriptor = m_descriptors.DSV[mipLevel];
			if (descriptor) return;

			const auto desc = (*this)->GetDesc();
			D3D12_DEPTH_STENCIL_VIEW_DESC DSVDesc{ .Format = desc.Format };
			switch (desc.Dimension) {
				case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
				{
					if (desc.DepthOrArraySize) {
						DSVDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1DARRAY;
						DSVDesc.Texture1DArray = {
							.MipSlice = mipLevel,
							.ArraySize = desc.DepthOrArraySize
						};
					}
					else {
						DSVDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1D;
						DSVDesc.Texture1D.MipSlice = mipLevel;
					}
				}
				break;

				case D3D12_RESOURCE_DIMENSION_TEXTURE2D:
				{
					if (desc.SampleDesc.Count) {
						if (desc.DepthOrArraySize) {
							DSVDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
							DSVDesc.Texture2DMSArray.ArraySize = desc.DepthOrArraySize;
						}
						else DSVDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
					}
					else if (desc.DepthOrArraySize) {
						DSVDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
						DSVDesc.Texture2DArray = {
							.MipSlice = mipLevel,
							.ArraySize = desc.DepthOrArraySize
						};
					}
					else {
						DSVDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
						DSVDesc.Texture2D.MipSlice = mipLevel;
					}
				}
				break;

				default: Throw<runtime_error>("Texture dimension not supported");

			}

			descriptor = m_deviceContext.DepthStencilDescriptorHeap->Allocate();
			m_deviceContext.Device->CreateDepthStencilView(*this, &DSVDesc, *descriptor);
		}

	private:
		Color m_clearColor;

		struct {
			unique_ptr<Descriptor> SRV;
			vector<unique_ptr<Descriptor>> UAV, RTV, DSV;
		} m_descriptors;
	};
}
