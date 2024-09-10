module;

#include "xess/xess_d3d12.h"

export module XeSS;

import CommandList;
import DeviceContext;

using namespace DirectX;

export {
	enum class XeSSResourceType { Depth, Velocity, ExposureScale, ResponsivePixelMask, Color, Output };

	struct XeSSSettings {
		xess_2d_t InputSize{};
		float Jitter[2]{};
		float ExposureScale = 1;
		bool Reset{};
	};

	struct XeSS {
		XeSS(const DeviceContext& deviceContext, xess_2d_t outputResolution, uint32_t flags) : m_outputResolution(outputResolution) {
			if (xessD3D12CreateContext(deviceContext.Device, &m_context) == XESS_RESULT_SUCCESS) {
				const xess_d3d12_init_params_t initParams{
					.outputResolution = outputResolution,
					.qualitySetting = XESS_QUALITY_SETTING_ULTRA_PERFORMANCE,
					.initFlags = flags
				};
				m_isAvailable = xessD3D12Init(m_context, &initParams) == XESS_RESULT_SUCCESS;
			}
		}

		~XeSS() { if (m_context != nullptr) xessDestroyContext(m_context); }

		auto IsAvailable() const { return m_isAvailable; }

		auto GetInputResolution(xess_quality_settings_t quality, xess_2d_t& resolution) const {
			return xessGetInputResolution(m_context, &m_outputResolution, quality, &resolution);
		}

		void Tag(XeSSResourceType type, ID3D12Resource* pResource) {
			switch (type) {
				case XeSSResourceType::Depth: m_executeParams.pDepthTexture = pResource; break;
				case XeSSResourceType::Velocity: m_executeParams.pVelocityTexture = pResource; break;
				case XeSSResourceType::ExposureScale: m_executeParams.pExposureScaleTexture = pResource; break;
				case XeSSResourceType::ResponsivePixelMask: m_executeParams.pResponsivePixelMaskTexture = pResource; break;
				case XeSSResourceType::Color: m_executeParams.pColorTexture = pResource; break;
				case XeSSResourceType::Output: m_executeParams.pOutputTexture = pResource; break;
			}
		}

		auto SetJitterScale(float x, float y) { return xessSetJitterScale(m_context, x, y); }
		auto SetVelocityScale(float x, float y) { return xessSetVelocityScale(m_context, x, y); }

		void SetConstants(const XeSSSettings& settings) {
			m_executeParams.jitterOffsetX = settings.Jitter[0];
			m_executeParams.jitterOffsetY = settings.Jitter[1];
			m_executeParams.exposureScale = settings.ExposureScale;
			m_executeParams.resetHistory = settings.Reset;
			m_executeParams.inputWidth = settings.InputSize.x;
			m_executeParams.inputHeight = settings.InputSize.y;
		}

		auto Execute(CommandList& commandList) { return xessD3D12Execute(m_context, commandList, &m_executeParams); }

	private:
		xess_2d_t m_outputResolution;

		bool m_isAvailable{};
		xess_context_handle_t m_context{};

		xess_d3d12_execute_params_t m_executeParams{};
	};
}
