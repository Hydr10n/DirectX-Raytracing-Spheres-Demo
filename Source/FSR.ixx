module;

#include "ffx_fsr2.h"
#include "dx12/ffx_fsr2_dx12.h"

#include <memory>

export module FSR;

using namespace std;

export {
	enum class FSRResourceType { Depth, MotionVectors, Exposure, Color, Output };

	struct FSRSettings {
		FfxDimensions2D RenderSize{};
		FfxFloatCoords2D MotionVectorScale{ 1, 1 };
		struct {
			FfxFloatCoords2D Jitter{};
			float Near{}, Far{}, VerticalFOV{};
		} Camera;
		float ElapsedMilliseconds{};
		bool Reset{};
	};

	struct FSR {
		FSR(const FSR&) = delete;
		FSR& operator=(const FSR&) = delete;

		FSR(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList, FfxDimensions2D outputSize, uint32_t flags, FfxFsr2Message message = nullptr) {
			const auto scratchBufferSize = ffxFsr2GetScratchMemorySizeDX12();
			m_scratchBuffer = make_unique<uint8_t[]>(scratchBufferSize);
			if (FfxFsr2Interface fsr2Interface{}; ffxFsr2GetInterfaceDX12(&fsr2Interface, pDevice, m_scratchBuffer.get(), scratchBufferSize) == FFX_OK) {
				const FfxFsr2ContextDescription contextDescription{
					.flags = flags,
					.maxRenderSize = outputSize,
					.displaySize = outputSize,
					.callbacks = fsr2Interface,
					.device = ffxGetDeviceDX12(pDevice),
					.fpMessage = message
				};
				m_isAvailable = ffxFsr2ContextCreate(&m_context, &contextDescription) == FFX_OK;
				m_dispatchDescription.commandList = ffxGetCommandListDX12(pCommandList);
			}
		}

		~FSR() { if (m_isAvailable) ffxFsr2ContextDestroy(&m_context); }

		auto IsAvailable() const { return m_isAvailable; }

		auto Tag(FSRResourceType type, ID3D12Resource* pResource, D3D12_RESOURCE_STATES state) {
			FfxResourceStates resourceState;
			if (state & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) resourceState = FFX_RESOURCE_STATE_UNORDERED_ACCESS;
			else if (state & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) resourceState = FFX_RESOURCE_STATE_COMPUTE_READ;
			else return false;
			switch (const auto resource = ffxGetResourceDX12(&m_context, pResource, nullptr, resourceState); type) {
				case FSRResourceType::Depth: m_dispatchDescription.depth = resource; break;
				case FSRResourceType::MotionVectors: m_dispatchDescription.motionVectors = resource; break;
				case FSRResourceType::Exposure: m_dispatchDescription.exposure = resource; break;
				case FSRResourceType::Color: m_dispatchDescription.color = resource; break;
				case FSRResourceType::Output: m_dispatchDescription.output = resource; break;
				default: throw;
			}
			return true;
		}

		void UpdateSettings(const FSRSettings& settings) {
			m_dispatchDescription.renderSize = settings.RenderSize;
			m_dispatchDescription.motionVectorScale = settings.MotionVectorScale;
			m_dispatchDescription.jitterOffset = settings.Camera.Jitter;
			m_dispatchDescription.cameraNear = settings.Camera.Near;
			m_dispatchDescription.cameraFar = settings.Camera.Far;
			m_dispatchDescription.cameraFovAngleVertical = settings.Camera.VerticalFOV;
			m_dispatchDescription.frameTimeDelta = settings.ElapsedMilliseconds;
			m_dispatchDescription.reset = settings.Reset;
		}

		auto Dispatch() { return ffxFsr2ContextDispatch(&m_context, &m_dispatchDescription); }

	private:
		bool m_isAvailable{};
		unique_ptr<uint8_t[]> m_scratchBuffer;
		FfxFsr2Context m_context{};
		FfxFsr2DispatchDescription m_dispatchDescription{};
	};
}
