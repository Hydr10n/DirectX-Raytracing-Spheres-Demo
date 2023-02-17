module;

#include "NRDIntegration.h"
#include "Extensions/NRIWrapperD3D12.h"

#include <d3d12.h>

#include <format>

#include <ranges>

#include <stacktrace>

export module NRD;

using namespace nrd;
using namespace nri;
using namespace std;

export {
	constexpr auto ToDXGIFormat(NormalEncoding value) {
		switch (value) {
			case NormalEncoding::RGBA8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
			case NormalEncoding::RGBA8_SNORM: return DXGI_FORMAT_R8G8B8A8_SNORM;
			case NormalEncoding::R10_G10_B10_A2_UNORM: return DXGI_FORMAT_R10G10B10A2_UNORM;
			case NormalEncoding::RGBA16_UNORM: return DXGI_FORMAT_R16G16B16A16_UNORM;
			case NormalEncoding::RGBA16_SNORM: return DXGI_FORMAT_R16G16B16A16_SNORM;
			default: return DXGI_FORMAT_UNKNOWN;
		}
	}

	class NRD {
	public:
		NRD(const NRD&) = delete;
		NRD& operator=(const NRD&) = delete;

		NRD(
			ID3D12Device* pDevice, ID3D12CommandQueue* pCommandQueue, ID3D12CommandAllocator* pCommandAllocator, ID3D12GraphicsCommandList* pCommandList,
			uint32_t backBufferCount,
			span<const Method> methods, const SIZE& outputSize
		) : m_NRD(backBufferCount) {
			const DeviceCreationD3D12Desc deviceCreationDesc{ .d3d12Device = pDevice, .d3d12GraphicsQueue = pCommandQueue };
			const CommandBufferD3D12Desc commandBufferDesc{ .d3d12CommandList = pCommandList, .d3d12CommandAllocator = pCommandAllocator };
			m_isNRIAvailable = CreateDeviceFromD3D12Device(deviceCreationDesc, m_device) == nri::Result::SUCCESS
				&& GetInterface(*m_device, NRI_INTERFACE(nri::CoreInterface), static_cast<CoreInterface*>(&m_NRI)) == nri::Result::SUCCESS
				&& GetInterface(*m_device, NRI_INTERFACE(nri::HelperInterface), static_cast<HelperInterface*>(&m_NRI)) == nri::Result::SUCCESS
				&& GetInterface(*m_device, NRI_INTERFACE(nri::WrapperD3D12Interface), static_cast<WrapperD3D12Interface*>(&m_NRI)) == nri::Result::SUCCESS
				&& m_NRI.CreateCommandBufferD3D12(*m_device, commandBufferDesc, m_commandBuffer) == nri::Result::SUCCESS;
			if (m_isNRIAvailable) {
				vector<MethodDesc> methodDescs;
				for (const auto& method : methods) methodDescs.emplace_back(method, static_cast<uint16_t>(outputSize.cx), static_cast<uint16_t>(outputSize.cy));
				const DenoiserCreationDesc denoiserCreationDesc{ .requestedMethods = data(methodDescs), .requestedMethodsNum = static_cast<uint32_t>(size(methodDescs)) };
				m_isNRDAvailable = m_NRD.Initialize(denoiserCreationDesc, *m_device, m_NRI, m_NRI);
			}
		}

		~NRD() {
			if (m_isNRDAvailable) m_NRD.Destroy();

			for (auto& textureTransitionBarrierDesc : m_textureTransitionBarrierDescs | views::values) {
				if (textureTransitionBarrierDesc.texture != nullptr) m_NRI.DestroyTexture(const_cast<Texture&>(*textureTransitionBarrierDesc.texture));
			}

			if (m_commandBuffer != nullptr) m_NRI.DestroyCommandBuffer(*m_commandBuffer);

			if (m_device != nullptr) DestroyDevice(*m_device);
		}

		auto IsAvailable() const { return m_isNRDAvailable; }

		auto SetResource(ResourceType type, ID3D12Resource* pResource, D3D12_RESOURCE_STATES state) {
			if (!m_isNRIAvailable) return false;

			TextureTransitionBarrierDescEx temp{};

			if (state & D3D12_RESOURCE_STATE_RENDER_TARGET) {
				temp.initialAccess = temp.nextAccess |= AccessBits::COLOR_ATTACHMENT;
				temp.initialLayout = temp.nextLayout = TextureLayout::COLOR_ATTACHMENT;
			}
			else if (state & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) temp.initialAccess = temp.nextAccess |= AccessBits::SHADER_RESOURCE_STORAGE;
			else if (state & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) {
				temp.initialAccess = temp.nextAccess |= AccessBits::SHADER_RESOURCE;
				temp.initialLayout = temp.nextLayout = TextureLayout::SHADER_RESOURCE;
			}
			else return false;

			if (m_NRI.CreateTextureD3D12(*m_device, { pResource }, const_cast<Texture*&>(temp.texture)) != nri::Result::SUCCESS) return false;

			auto& textureTransitionBarrierDesc = m_textureTransitionBarrierDescs[type];
			if (textureTransitionBarrierDesc.texture != nullptr) m_NRI.DestroyTexture(const_cast<Texture&>(*textureTransitionBarrierDesc.texture));
			textureTransitionBarrierDesc = temp;

			NrdIntegration_SetResource(m_userPool, type, { &textureTransitionBarrierDesc, ConvertDXGIFormatToNRI(pResource->GetDesc().Format) });

			return true;
		}

		auto SetMethodSettings(Method method, const void* methodSettings) { return m_NRD.SetMethodSettings(method, methodSettings); }

		void Denoise(uint32_t consecutiveFrameIndex, const CommonSettings& commonSettings) {
			m_NRD.Denoise(consecutiveFrameIndex, *m_commandBuffer, commonSettings, m_userPool, true);

			vector<TextureTransitionBarrierDesc> textureTransitionBarrierDescs;
			textureTransitionBarrierDescs.reserve(size(m_textureTransitionBarrierDescs));
			for (auto& textureTransitionBarrierDesc : m_textureTransitionBarrierDescs | views::values) {
				if (textureTransitionBarrierDesc.nextAccess != textureTransitionBarrierDesc.initialAccess
					&& textureTransitionBarrierDesc.nextLayout != textureTransitionBarrierDesc.initialLayout) {
					textureTransitionBarrierDesc.prevAccess = textureTransitionBarrierDesc.nextAccess;
					textureTransitionBarrierDesc.nextAccess = textureTransitionBarrierDesc.initialAccess;
					textureTransitionBarrierDesc.prevLayout = textureTransitionBarrierDesc.nextLayout;
					textureTransitionBarrierDesc.nextLayout = textureTransitionBarrierDesc.initialLayout;
					textureTransitionBarrierDescs.emplace_back(textureTransitionBarrierDesc);
				}
			}
			const TransitionBarrierDesc transitionBarrierDesc{ .textures = data(textureTransitionBarrierDescs), .textureNum = static_cast<uint32_t>(size(textureTransitionBarrierDescs)) };
			m_NRI.CmdPipelineBarrier(*m_commandBuffer, &transitionBarrierDesc, nullptr, BarrierDependency::ALL_STAGES);
		}

		auto GetTotalMemoryUsageInMb() const { return m_NRD.GetTotalMemoryUsageInMb(); }
		auto GetPersistentMemoryUsageInMb() const { return m_NRD.GetPersistentMemoryUsageInMb(); }
		auto GetAliasableMemoryUsageInMb() const { return m_NRD.GetAliasableMemoryUsageInMb(); }

	private:
		bool m_isNRDAvailable{};
		NrdUserPool m_userPool{};
		NrdIntegration m_NRD;

		bool m_isNRIAvailable{};
		Device* m_device{};
		CommandBuffer* m_commandBuffer{};
		struct NriInterface : CoreInterface, HelperInterface, WrapperD3D12Interface {} m_NRI{};

		struct TextureTransitionBarrierDescEx : TextureTransitionBarrierDesc {
			AccessBits initialAccess;
			TextureLayout initialLayout;
		};
		map<ResourceType, TextureTransitionBarrierDescEx> m_textureTransitionBarrierDescs;
	};
}
