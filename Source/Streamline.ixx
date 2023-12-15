module;

#include "sl_helpers.h"
#include "sl_matrix_helpers.h"

#include <ranges>
#include <set>
#include <vector>

export module Streamline;

using namespace sl;
using namespace std;

export {
	struct Streamline {
		static constexpr Feature Features[]{ kFeatureDLSS, kFeatureNIS };

		Streamline(const Streamline&) = delete;
		Streamline& operator=(const Streamline&) = delete;

		Streamline(void* commandBuffer, void* adapterLUID, size_t adapterLUIDSize, uint32_t viewportID) : m_commandBuffer(commandBuffer), m_viewport(viewportID) {
			if (_.IsInitialized) {
				for (const auto feature : Features) {
					AdapterInfo adapterInfo;
					adapterInfo.deviceLUID = static_cast<uint8_t*>(adapterLUID);
					adapterInfo.deviceLUIDSizeInBytes = adapterLUIDSize;
					if (slIsFeatureSupported(feature, adapterInfo) == Result::eOk) m_features.emplace(feature);
				}
			}
		}

		static auto IsInitialized() { return _.IsInitialized; }

		auto IsFeatureAvailable(Feature feature) const { return m_features.contains(feature); }

		auto NewFrame(const uint32_t* index = nullptr) { return slGetNewFrameToken(m_currentFrame, index); }

		auto Tag(span<const ResourceTag> resourceTags) const { return slSetTag(m_viewport, data(resourceTags), static_cast<uint32_t>(size(resourceTags)), m_commandBuffer); }

		template <typename T>
		auto SetConstants(const T& constants) const {
			if constexpr (is_same_v<T, Constants>) return slSetConstants(constants, *m_currentFrame, m_viewport);
			if constexpr (is_same_v<T, DLSSOptions>) return slDLSSSetOptions(m_viewport, constants);
			if constexpr (is_same_v<T, NISOptions>) return slNISSetOptions(m_viewport, constants);
			throw;
		}

		auto EvaluateFeature(Feature feature, span<const ResourceTag> resourceTags = {}) const {
			vector<const BaseStructure*> inputs{ &m_viewport };
			for (const auto& resourceTag : resourceTags) inputs.emplace_back(&resourceTag);
			return slEvaluateFeature(feature, *m_currentFrame, data(inputs), static_cast<uint32_t>(size(inputs)), m_commandBuffer);
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

		void* m_commandBuffer;

		ViewportHandle m_viewport;

		set<Feature> m_features;

		FrameToken* m_currentFrame{};
	};

	struct ResourceTagInfo {
		BufferType Type;
		Resource Resource;
		Extent Extent;
		ResourceLifecycle Lifecycle;

		operator ResourceTag() { return ResourceTag(&Resource, Type, Lifecycle, &Extent); }
	};

	auto CreateResourceTags(span<ResourceTagInfo> resourceTagInfos) {
		vector<ResourceTag> resourceTags;
		for (auto& resourceTagInfo : resourceTagInfos) resourceTags.emplace_back(resourceTagInfo);
		return resourceTags;
	}

	namespace sl {
		void recalculateCameraMatrices(Constants& values, const float4x4& cameraViewToWorldPrev, const float4x4& cameraViewToClipPrev) {
			vectorNormalize(values.cameraRight);
			vectorNormalize(values.cameraFwd);
			vectorCrossProduct(values.cameraUp, values.cameraFwd, values.cameraRight);
			vectorNormalize(values.cameraUp);

			const float4x4 cameraViewToWorld{
				float4(values.cameraRight.x, values.cameraRight.y, values.cameraRight.z, 0),
				float4(values.cameraUp.x, values.cameraUp.y, values.cameraUp.z, 0),
				float4(values.cameraFwd.x, values.cameraFwd.y, values.cameraFwd.z, 0),
				float4(values.cameraPos.x, values.cameraPos.y, values.cameraPos.z, 1)
			};

			float4x4 cameraViewToPrevCameraView;
			calcCameraToPrevCamera(cameraViewToPrevCameraView, cameraViewToWorld, cameraViewToWorldPrev);

			matrixFullInvert(values.clipToCameraView, values.cameraViewToClip);

			float4x4 clipToPrevCameraView;
			matrixMul(clipToPrevCameraView, values.clipToCameraView, cameraViewToPrevCameraView);
			matrixMul(values.clipToPrevClip, clipToPrevCameraView, cameraViewToClipPrev);
			matrixFullInvert(values.prevClipToClip, values.clipToPrevClip);
		}
	}
}
