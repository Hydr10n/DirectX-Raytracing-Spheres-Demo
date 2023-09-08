module;

#include "NRDIntegration.h"
#include "Extensions/NRIWrapperD3D12.h"

#include <d3d12.h>

#include <ranges>

export module NRD;

using namespace nrd;
using namespace nri;
using namespace std;

export class NRD {
public:
	NRD(const NRD&) = delete;
	NRD& operator=(const NRD&) = delete;

	NRD(
		ID3D12Device* pDevice, ID3D12CommandQueue* pCommandQueue, ID3D12GraphicsCommandList* pCommandList,
		uint32_t backBufferCount,
		span<const DenoiserDesc> denoiserDescs
	) : m_NRD(backBufferCount) {
		if (CreateDeviceFromD3D12Device(DeviceCreationD3D12Desc{ .d3d12Device = pDevice, .d3d12GraphicsQueue = pCommandQueue }, m_device) == nri::Result::SUCCESS
			&& GetInterface(*m_device, NRI_INTERFACE(nri::CoreInterface), static_cast<CoreInterface*>(&m_NRI)) == nri::Result::SUCCESS
			&& GetInterface(*m_device, NRI_INTERFACE(nri::HelperInterface), static_cast<HelperInterface*>(&m_NRI)) == nri::Result::SUCCESS
			&& GetInterface(*m_device, NRI_INTERFACE(nri::WrapperD3D12Interface), static_cast<WrapperD3D12Interface*>(&m_NRI)) == nri::Result::SUCCESS
			&& m_NRI.CreateCommandBufferD3D12(*m_device, CommandBufferD3D12Desc{ .d3d12CommandList = pCommandList }, m_commandBuffer) == nri::Result::SUCCESS) {
			const InstanceCreationDesc denoiserCreationDesc{ .denoisers = data(denoiserDescs), .denoisersNum = static_cast<uint32_t>(size(denoiserDescs)) };
			m_isAvailable = m_NRD.Initialize(denoiserCreationDesc, *m_device, m_NRI, m_NRI);
			if (m_isAvailable) m_textureTransitionBarrierDescs.resize(NrdUserPool().max_size());
		}
	}

	~NRD() {
		if (m_isAvailable) m_NRD.Destroy();

		for (const auto& textureTransitionBarrierDesc : m_textureTransitionBarrierDescs) {
			if (textureTransitionBarrierDesc.texture != nullptr) m_NRI.DestroyTexture(const_cast<Texture&>(*textureTransitionBarrierDesc.texture));
		}

		if (m_commandBuffer != nullptr) m_NRI.DestroyCommandBuffer(*m_commandBuffer);

		if (m_device != nullptr) DestroyDevice(*m_device);
	}

	bool IsAvailable() const { return m_isAvailable; }

	void NewFrame(uint32_t frameIndex) { m_NRD.NewFrame(frameIndex); }

	bool Tag(ResourceType type, ID3D12Resource* pResource, D3D12_RESOURCE_STATES state) {
		if (!m_isAvailable || pResource == nullptr) return false;

		AccessBits accessBits;
		TextureLayout textureLayout;
		if (state & D3D12_RESOURCE_STATE_RENDER_TARGET) {
			accessBits = AccessBits::COLOR_ATTACHMENT;
			textureLayout = TextureLayout::COLOR_ATTACHMENT;
		}
		else if (state & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
			accessBits = AccessBits::SHADER_RESOURCE_STORAGE;
			textureLayout = TextureLayout::GENERAL;
		}
		else if (state & D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE) {
			accessBits = AccessBits::SHADER_RESOURCE;
			textureLayout = TextureLayout::SHADER_RESOURCE;
		}
		else return false;

		auto& textureTransitionBarrierDesc = m_textureTransitionBarrierDescs[static_cast<size_t>(type)];

		textureTransitionBarrierDesc.initialAccess = textureTransitionBarrierDesc.nextAccess = accessBits;
		textureTransitionBarrierDesc.initialLayout = textureTransitionBarrierDesc.nextLayout = textureLayout;

		if (textureTransitionBarrierDesc.resource == pResource) return true;

		Texture* texture;
		if (m_NRI.CreateTextureD3D12(*m_device, { pResource }, texture) != nri::Result::SUCCESS) return false;
		if (textureTransitionBarrierDesc.texture != nullptr) m_NRI.DestroyTexture(const_cast<Texture&>(*textureTransitionBarrierDesc.texture));
		textureTransitionBarrierDesc.texture = texture;

		NrdIntegration_SetResource(m_userPool, type, { &textureTransitionBarrierDesc, ConvertDXGIFormatToNRI(pResource->GetDesc().Format) });

		textureTransitionBarrierDesc.resource = pResource;

		return true;
	}

	bool SetCommonSettings(const CommonSettings& commonSettings) { return m_NRD.SetCommonSettings(commonSettings); }

	template <typename T>
	bool SetDenoiserSettings(Identifier denoiser, const T& denoiserSettings) { return m_NRD.SetDenoiserSettings(denoiser, &denoiserSettings); }

	void Denoise(span<const Identifier> denoisers) {
		m_NRD.Denoise(data(denoisers), static_cast<uint32_t>(size(denoisers)), *m_commandBuffer, m_userPool, true);

		vector<TextureTransitionBarrierDesc> textureTransitionBarrierDescs;
		for (auto& TextureTransitionBarrierDesc : m_textureTransitionBarrierDescs) {
			if (TextureTransitionBarrierDesc.texture != nullptr
				&& TextureTransitionBarrierDesc.nextAccess != TextureTransitionBarrierDesc.initialAccess
				&& TextureTransitionBarrierDesc.nextLayout != TextureTransitionBarrierDesc.initialLayout) {
				TextureTransitionBarrierDesc.prevAccess = TextureTransitionBarrierDesc.nextAccess;
				TextureTransitionBarrierDesc.nextAccess = TextureTransitionBarrierDesc.initialAccess;
				TextureTransitionBarrierDesc.prevLayout = TextureTransitionBarrierDesc.nextLayout;
				TextureTransitionBarrierDesc.nextLayout = TextureTransitionBarrierDesc.initialLayout;
				textureTransitionBarrierDescs.emplace_back(TextureTransitionBarrierDesc);
			}
		}
		const TransitionBarrierDesc transitionBarrierDesc{ .textures = data(textureTransitionBarrierDescs), .textureNum = static_cast<uint32_t>(size(textureTransitionBarrierDescs)) };
		m_NRI.CmdPipelineBarrier(*m_commandBuffer, &transitionBarrierDesc, nullptr, BarrierDependency::ALL_STAGES);
	}

	double GetTotalMemoryUsageInMb() const { return m_NRD.GetTotalMemoryUsageInMb(); }
	double GetPersistentMemoryUsageInMb() const { return m_NRD.GetPersistentMemoryUsageInMb(); }
	double GetAliasableMemoryUsageInMb() const { return m_NRD.GetAliasableMemoryUsageInMb(); }

	static constexpr DXGI_FORMAT ToDXGIFormat(NormalEncoding value) {
		switch (value) {
			case NormalEncoding::RGBA8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
			case NormalEncoding::RGBA8_SNORM: return DXGI_FORMAT_R8G8B8A8_SNORM;
			case NormalEncoding::R10_G10_B10_A2_UNORM: return DXGI_FORMAT_R10G10B10A2_UNORM;
			case NormalEncoding::RGBA16_UNORM: return DXGI_FORMAT_R16G16B16A16_UNORM;
			case NormalEncoding::RGBA16_SNORM: return DXGI_FORMAT_R16G16B16A16_SNORM;
			default: return DXGI_FORMAT_UNKNOWN;
		}
	}

private:
	bool m_isAvailable{};
	NrdUserPool m_userPool{};
	NrdIntegration m_NRD;

	Device* m_device{};
	CommandBuffer* m_commandBuffer{};
	struct NriInterface : CoreInterface, HelperInterface, WrapperD3D12Interface {} m_NRI{};

	struct TextureTransitionBarrierDescEx : TextureTransitionBarrierDesc {
		ID3D12Resource* resource;
		AccessBits initialAccess;
		TextureLayout initialLayout;
	};
	vector<TextureTransitionBarrierDescEx> m_textureTransitionBarrierDescs;
};
