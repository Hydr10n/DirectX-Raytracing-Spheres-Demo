#pragma once

#include <fstream>

#include <Windows.h>

#include "JsonHelpers.h"

#include "sl_helpers.h"

#include "directxtk12/PostProcess.h"

import DisplayHelpers;
import FSR;
import NRD;
import Streamline;
import WindowHelpers;

JSON_CONVERSION1_FUNCTIONS(SIZE, ("Width", cx), ("Height", cy));

namespace WindowHelpers {
	NLOHMANN_JSON_SERIALIZE_ENUM(
		WindowMode,
		{
			{ WindowMode::Windowed, "Windowed" },
			{ WindowMode::Borderless, "Borderless" },
			{ WindowMode::Fullscreen, "Fullscreen" }
		}
	);
}

namespace sl {
	NLOHMANN_JSON_SERIALIZE_ENUM(
		ReflexMode,
		{
			{ ReflexMode::eOff, "Off" },
			{ ReflexMode::eLowLatency, "LowLatency" },
			{ ReflexMode::eLowLatencyWithBoost, "LowLatencyWithBoost" }
		}
	);
}

NLOHMANN_JSON_SERIALIZE_ENUM(
	NRDDenoiser,
	{
		{ NRDDenoiser::None, "None" },
		{ NRDDenoiser::ReBLUR, "ReBLUR" },
		{ NRDDenoiser::ReLAX, "ReLAX" }
	}
);

enum class Upscaler { None, DLSS, FSR };

NLOHMANN_JSON_SERIALIZE_ENUM(
	Upscaler,
	{
		{ Upscaler::None, "None" },
		{ Upscaler::DLSS, "DLSS" },
		{ Upscaler::FSR, "FSR" }
	}
);

enum class SuperResolutionMode { Auto, Native, Quality, Balanced, Performance, UltraPerformance };

NLOHMANN_JSON_SERIALIZE_ENUM(
	SuperResolutionMode,
	{
		{ SuperResolutionMode::Auto, "Auto" },
		{ SuperResolutionMode::Native, "Native" },
		{ SuperResolutionMode::Quality, "Quality" },
		{ SuperResolutionMode::Balanced, "Balanced" },
		{ SuperResolutionMode::Performance, "Performance" },
		{ SuperResolutionMode::UltraPerformance, "UltraPerformance" }
	}
);

namespace DirectX {
	NLOHMANN_JSON_SERIALIZE_ENUM(
		ToneMapPostProcess::Operator,
		{
			{ ToneMapPostProcess::None, "None" },
			{ ToneMapPostProcess::Saturate, "Saturate" },
			{ ToneMapPostProcess::Reinhard, "Reinhard" },
			{ ToneMapPostProcess::ACESFilmic, "ACESFilmic" }
		}
	);

	NLOHMANN_JSON_SERIALIZE_ENUM(
		ToneMapPostProcess::ColorPrimaryRotation,
		{
			{ ToneMapPostProcess::HDTV_to_UHDTV, "HDTV_to_UHDTV" },
			{ ToneMapPostProcess::DCI_P3_D65_to_UHDTV, "DCI_P3_D65_to_UHDTV" },
			{ ToneMapPostProcess::HDTV_to_DCI_P3_D65, "HDTV_to_DCI_P3_D65" }
		}
	);
}

class MyAppData {
	template <typename T>
	struct Data {
		bool Load() {
			try {
				ordered_json_f json;
				std::ifstream(T::FilePath) >> json;
				reinterpret_cast<T&>(*this) = json;
				return true;
			}
			catch (...) { return false; }
		}

		bool Save() const {
			try {
				std::filesystem::create_directories(std::filesystem::path(T::FilePath).remove_filename());
				std::ofstream(T::FilePath, std::ios_base::trunc) << std::setw(4) << ordered_json_f(reinterpret_cast<const T&>(*this));
				return true;
			}
			catch (...) { return false; }
		}

		virtual void Check() = 0;
	};

public:
	struct Settings {
		inline static const std::filesystem::path DirectoryPath = std::filesystem::path(*__wargv).replace_filename(L"Settings");

		inline static struct Graphics : Data<Graphics> {
			inline static const std::filesystem::path FilePath = DirectoryPath / L"Graphics.json";

			WindowHelpers::WindowMode WindowMode{};

			DisplayHelpers::Resolution Resolution{};

			bool IsHDREnabled = true, IsVSyncEnabled{};

			sl::ReflexMode ReflexMode = sl::ReflexMode::eLowLatency;

			struct Camera {
				static constexpr float MinHorizontalFieldOfView = 30, MaxHorizontalFieldOfView = 120;

				bool IsJitterEnabled = true;

				float HorizontalFieldOfView = 90;

				FRIEND_JSON_CONVERSION_FUNCTIONS(Camera, IsJitterEnabled, HorizontalFieldOfView);
			} Camera;

			struct Raytracing {
				static constexpr UINT MaxBounces = 32, MaxSamplesPerPixel = 16;

				bool IsRussianRouletteEnabled = false;

				UINT Bounces = 8, SamplesPerPixel = 1;

				struct RTXDI {
					static constexpr UINT MaxLocalLightSamples = 32, MaxBRDFSamples = 16, MaxSpatioTemporalSamples = 4;

					bool IsEnabled = true;

					UINT LocalLightSamples = 8, BRDFSamples = 1, SpatioTemporalSamples = 2;

					FRIEND_JSON_CONVERSION_FUNCTIONS(RTXDI, IsEnabled, LocalLightSamples, BRDFSamples, SpatioTemporalSamples);
				} RTXDI;

				FRIEND_JSON_CONVERSION_FUNCTIONS(Raytracing, IsRussianRouletteEnabled, Bounces, SamplesPerPixel, RTXDI);
			} Raytracing;

			struct PostProcessing {
				struct NRD {
					NRDDenoiser Denoiser = NRDDenoiser::ReLAX;

					bool IsValidationOverlayEnabled{};

					FRIEND_JSON_CONVERSION_FUNCTIONS(NRD, Denoiser, IsValidationOverlayEnabled);
				} NRD;

				struct SuperResolution {
					Upscaler Upscaler = Upscaler::DLSS;

					SuperResolutionMode Mode = SuperResolutionMode::Auto;

					FRIEND_JSON_CONVERSION_FUNCTIONS(SuperResolution, Upscaler, Mode);
				} SuperResolution;

				bool IsDLSSFrameGenerationEnabled = true;

				struct NIS {
					bool IsEnabled{};

					float Sharpness = 0.5f;

					FRIEND_JSON_CONVERSION_FUNCTIONS(NIS, IsEnabled, Sharpness);
				} NIS;

				bool IsChromaticAberrationEnabled = true;

				struct Bloom {
					bool IsEnabled = true;

					float Strength = 0.05f;

					FRIEND_JSON_CONVERSION_FUNCTIONS(Bloom, IsEnabled, Strength);
				} Bloom;

				struct ToneMapping {
					struct HDR {
						static constexpr float MinPaperWhiteNits = 50, MaxPaperWhiteNits = 10000;

						float PaperWhiteNits = 200;

						DirectX::ToneMapPostProcess::ColorPrimaryRotation ColorPrimaryRotation = DirectX::ToneMapPostProcess::HDTV_to_UHDTV;

						FRIEND_JSON_CONVERSION_FUNCTIONS(HDR, PaperWhiteNits, ColorPrimaryRotation);
					} HDR;

					struct NonHDR {
						static constexpr float MinExposure = -10, MaxExposure = 10;

						DirectX::ToneMapPostProcess::Operator Operator = DirectX::ToneMapPostProcess::ACESFilmic;

						float Exposure{};

						FRIEND_JSON_CONVERSION_FUNCTIONS(NonHDR, Operator, Exposure);
					} NonHDR;

					FRIEND_JSON_CONVERSION_FUNCTIONS(ToneMapping, HDR, NonHDR);
				} ToneMapping;

				FRIEND_JSON_CONVERSION_FUNCTIONS(PostProcessing, NRD, SuperResolution, IsDLSSFrameGenerationEnabled, NIS, IsChromaticAberrationEnabled, Bloom, ToneMapping);
			} PostProcessing;

			FRIEND_JSON_CONVERSION_FUNCTIONS(Graphics, WindowMode, Resolution, IsHDREnabled, IsVSyncEnabled, ReflexMode, Camera, Raytracing, PostProcessing);

			void Check() override {
				using namespace std;

				Camera.HorizontalFieldOfView = clamp(Camera.HorizontalFieldOfView, Camera.MinHorizontalFieldOfView, Camera.MaxHorizontalFieldOfView);

				{
					Raytracing.Bounces = clamp(Raytracing.Bounces, 0u, Raytracing.MaxBounces);
					Raytracing.SamplesPerPixel = clamp(Raytracing.SamplesPerPixel, 1u, Raytracing.MaxSamplesPerPixel);

					Raytracing.RTXDI.LocalLightSamples = clamp(Raytracing.RTXDI.LocalLightSamples, 1u, Raytracing.RTXDI.MaxLocalLightSamples);
					Raytracing.RTXDI.BRDFSamples = clamp(Raytracing.RTXDI.BRDFSamples, 0u, Raytracing.RTXDI.MaxBRDFSamples);
					Raytracing.RTXDI.SpatioTemporalSamples = clamp(Raytracing.RTXDI.SpatioTemporalSamples, 0u, Raytracing.RTXDI.MaxSpatioTemporalSamples);
				}

				{
					PostProcessing.NIS.Sharpness = clamp(PostProcessing.NIS.Sharpness, 0.0f, 1.0f);

					PostProcessing.Bloom.Strength = clamp(PostProcessing.Bloom.Strength, 0.0f, 1.0f);

					PostProcessing.ToneMapping.HDR.PaperWhiteNits = clamp(PostProcessing.ToneMapping.HDR.PaperWhiteNits, PostProcessing.ToneMapping.HDR.MinPaperWhiteNits, PostProcessing.ToneMapping.HDR.MaxPaperWhiteNits);
					PostProcessing.ToneMapping.NonHDR.Exposure = clamp(PostProcessing.ToneMapping.NonHDR.Exposure, PostProcessing.ToneMapping.NonHDR.MinExposure, PostProcessing.ToneMapping.NonHDR.MaxExposure);
				}
			}
		} Graphics;

		inline static struct UI : Data<UI> {
			inline static const std::filesystem::path FilePath = DirectoryPath / L"UI.json";

			bool ShowOnStartup = true;

			float WindowOpacity = 0.5f;

			FRIEND_JSON_CONVERSION_FUNCTIONS(UI, ShowOnStartup, WindowOpacity);

			void Check() override {
				using namespace std;

				WindowOpacity = clamp(WindowOpacity, 0.0f, 1.0f);
			}
		} UI;

		inline static struct Controls : Data<Controls> {
			inline static const std::filesystem::path FilePath = DirectoryPath / L"Controls.json";

			struct Camera {
				struct Speed {
					static constexpr float MaxMovement = 1000, MaxRotation = 2;

					float Movement = 10, Rotation = 0.5f;

					FRIEND_JSON_CONVERSION_FUNCTIONS(Speed, Movement, Rotation);
				} Speed;

				FRIEND_JSON_CONVERSION_FUNCTIONS(Camera, Speed);
			} Camera;

			FRIEND_JSON_CONVERSION_FUNCTIONS(Controls, Camera);

			void Check() override {
				using namespace std;

				Camera.Speed.Movement = clamp(Camera.Speed.Movement, 0.0f, Camera.Speed.MaxMovement);
				Camera.Speed.Rotation = clamp(Camera.Speed.Rotation, 0.0f, Camera.Speed.MaxRotation);
			}
		} Controls;

	private:
		inline static const struct _ {
			_() {
				std::ignore = Graphics.Load();
				Graphics.Check();

				std::ignore = UI.Load();
				UI.Check();

				std::ignore = Controls.Load();
				Controls.Check();
			}

			~_() {
				std::ignore = Graphics.Save();
				std::ignore = UI.Save();
				std::ignore = Controls.Save();
			}
		} _;
	};
};
