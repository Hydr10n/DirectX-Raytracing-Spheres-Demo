module;

#include "sl_helpers.h"
#include "sl_matrix_helpers.h"

#include <d3d12.h>

#include <ranges>

#include <vector>
#include <set>

export module Streamline;

using namespace sl;
using namespace std;

export {
	class Streamline {
	public:
		static constexpr Feature Features[]{ kFeatureDLSS, kFeatureNIS };

		Streamline(const Streamline&) = delete;
		Streamline& operator=(const Streamline&) = delete;

		Streamline(uint32_t viewportID, LUID adapterLUID, ID3D12GraphicsCommandList* pCommandList) : m_viewport(viewportID), m_commandList(pCommandList) {
			if (_.IsInitialized) {
				for (const auto feature : Features) {
					AdapterInfo adapterInfo;
					adapterInfo.deviceLUID = reinterpret_cast<uint8_t*>(&adapterLUID);
					adapterInfo.deviceLUIDSizeInBytes = sizeof(adapterLUID);
					if (slIsFeatureSupported(feature, adapterInfo) == Result::eOk) m_features.emplace(feature);
				}
			}
		}

		static auto IsInitialized() { return _.IsInitialized; }

		auto IsFeatureAvailable(Feature feature) const { return m_features.contains(feature); }

		auto NewFrame(const uint32_t* index = nullptr) { return slGetNewFrameToken(m_currentFrame, index); }

		auto Tag(span<const ResourceTag> resourceTags) const { return slSetTag(m_viewport, data(resourceTags), static_cast<uint32_t>(size(resourceTags)), m_commandList); }

		template <typename T>
		auto SetConstants(const T& constants) const {
			Result ret;
			if constexpr (is_same_v<T, Constants>) ret = slSetConstants(constants, *m_currentFrame, m_viewport);
			else if constexpr (is_same_v<T, DLSSOptions>) ret = slDLSSSetOptions(m_viewport, constants);
			else if constexpr (is_same_v<T, NISOptions>) ret = slNISSetOptions(m_viewport, constants);
			else throw;
			return ret;
		}

		auto EvaluateFeature(Feature feature, span<const ResourceTag> resourceTags = {}) const {
			vector<const BaseStructure*> inputs{ &m_viewport };
			for (const auto& resourceTag : resourceTags) inputs.emplace_back(&resourceTag);
			return slEvaluateFeature(feature, *m_currentFrame, data(inputs), static_cast<uint32_t>(size(inputs)), m_commandList);
		}

	private:
		inline static const struct _ {
			bool IsInitialized{};

			_() {
				Preferences preferences;
#ifdef _DEBUG
				preferences.showConsole = true;
				preferences.logLevel = LogLevel::eVerbose;
#endif
				preferences.featuresToLoad = data(Features);
				preferences.numFeaturesToLoad = static_cast<uint32_t>(size(Features));
				preferences.applicationId = ~0u;
				IsInitialized = slInit(preferences) == Result::eOk;
			}

			~_() { if (IsInitialized) slShutdown(); }
		} _;

		ViewportHandle m_viewport;

		ID3D12GraphicsCommandList* m_commandList;

		set<Feature> m_features;

		FrameToken* m_currentFrame{};
	};

	struct ResourceTagInfo {
		BufferType Type;
		Resource Resource;
		Extent Extent;
		ResourceLifecycle Lifecycle;

		auto ToResourceTag() { return ResourceTag(&Resource, Type, Lifecycle, &Extent); }
	};

	auto CreateResourceTags(span<ResourceTagInfo> resourceTagInfos) {
		vector<ResourceTag> resourceTags;
		for (auto& resourceTagInfo : resourceTagInfos) resourceTags.emplace_back(resourceTagInfo.ToResourceTag());
		return resourceTags;
	}

	namespace sl {
		void recalculateCameraMatrices(Constants& values, const float4x4& cameraViewToWorldPrev, const float4x4& cameraViewToClipPrev) {
			vectorNormalize(values.cameraRight);
			vectorNormalize(values.cameraFwd);
			vectorCrossProduct(values.cameraUp, values.cameraFwd, values.cameraRight);
			vectorNormalize(values.cameraUp);
			float4x4 cameraViewToWorld = {
				float4(values.cameraRight.x, values.cameraRight.y, values.cameraRight.z, 0.f),
				float4(values.cameraUp.x,    values.cameraUp.y,    values.cameraUp.z,    0.f),
				float4(values.cameraFwd.x,   values.cameraFwd.y,   values.cameraFwd.z,   0.f),
				float4(values.cameraPos.x,   values.cameraPos.y,   values.cameraPos.z, 1.f)
			};
			matrixFullInvert(values.clipToCameraView, values.cameraViewToClip);

			float4x4 cameraViewToPrevCameraView;
			calcCameraToPrevCamera(cameraViewToPrevCameraView, cameraViewToWorld, cameraViewToWorldPrev);

			float4x4 clipToPrevCameraView;
			matrixMul(clipToPrevCameraView, values.clipToCameraView, cameraViewToPrevCameraView);
			matrixMul(values.clipToPrevClip, clipToPrevCameraView, cameraViewToClipPrev);
			matrixFullInvert(values.prevClipToClip, values.clipToPrevClip);
		}
	}
}
