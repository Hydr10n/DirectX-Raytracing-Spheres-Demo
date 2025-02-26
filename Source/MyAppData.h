#pragma once

#include <fstream>

#include <Windows.h>

#include "JSONHelpers.h"

#include "Rtxdi/DI/ReSTIRDIParameters.h"

#include "sl_helpers.h"

#include "directxtk12/PostProcess.h"

import Denoiser;
import DisplayHelpers;
import RTXGI;
import Upscaler;
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

NLOHMANN_JSON_SERIALIZE_ENUM(
	ReSTIRDI_LocalLightSamplingMode,
	{
		{ ReSTIRDI_LocalLightSamplingMode::Uniform, "Uniform" },
		{ ReSTIRDI_LocalLightSamplingMode::Power_RIS, "Power_RIS" },
		{ ReSTIRDI_LocalLightSamplingMode::ReGIR_RIS, "ReGIR_RIS" }
	}
);

NLOHMANN_JSON_SERIALIZE_ENUM(
	ReSTIRDI_TemporalBiasCorrectionMode,
	{
		{ ReSTIRDI_TemporalBiasCorrectionMode::Off, "Off" },
		{ ReSTIRDI_TemporalBiasCorrectionMode::Basic, "Basic" },
		{ ReSTIRDI_TemporalBiasCorrectionMode::Pairwise, "Pairwise" },
		{ ReSTIRDI_TemporalBiasCorrectionMode::Raytraced, "Raytraced" }
	}
);

NLOHMANN_JSON_SERIALIZE_ENUM(
	ReSTIRDI_SpatialBiasCorrectionMode,
	{
		{ ReSTIRDI_SpatialBiasCorrectionMode::Off, "Off" },
		{ ReSTIRDI_SpatialBiasCorrectionMode::Basic, "Basic" },
		{ ReSTIRDI_SpatialBiasCorrectionMode::Pairwise, "Pairwise" },
		{ ReSTIRDI_SpatialBiasCorrectionMode::Raytraced, "Raytraced" }
	}
);

NLOHMANN_JSON_SERIALIZE_ENUM(
	RTXGITechnique,
	{
		{ RTXGITechnique::None, "None" },
		{ RTXGITechnique::SHARC, "SHARC" }
	}
);

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
	Denoiser,
	{
		{ Denoiser::None, "None" },
		{ Denoiser::DLSSRayReconstruction, "DLSSRayReconstruction" },
		{ Denoiser::NRDReBLUR, "NRDReBLUR" },
		{ Denoiser::NRDReLAX, "NRDReLAX" }
	}
);

NLOHMANN_JSON_SERIALIZE_ENUM(
	Upscaler,
	{
		{ Upscaler::None, "None" },
		{ Upscaler::DLSS, "DLSS" },
		{ Upscaler::XeSS, "XeSS" }
	}
);

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
		auto Load() {
			try {
				ordered_json_f json;
				std::ifstream(T::FilePath) >> json;
				reinterpret_cast<T&>(*this) = json;
				return true;
			}
			catch (...) { return false; }
		}

		auto Save() const {
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
				bool IsJitterEnabled = true;

				static constexpr float MinHorizontalFieldOfView = 30, MaxHorizontalFieldOfView = 120;
				float HorizontalFieldOfView = 90;

				FRIEND_JSON_CONVERSION_FUNCTIONS(Camera, IsJitterEnabled, HorizontalFieldOfView);
			} Camera;

			struct Raytracing {
				bool IsRussianRouletteEnabled = true;

				static constexpr uint32_t MaxBounces = 100, MaxSamplesPerPixel = 16;
				uint32_t Bounces = 8, SamplesPerPixel = 1;

				bool IsShaderExecutionReorderingEnabled = true;

				struct RTXDI {
					struct ReSTIRDI {
						bool IsEnabled = true;

						struct ReGIR {
							struct Cell {
								static constexpr float MinSize = 0.1f, MaxSize = 10;
								float Size = 1;

								bool IsVisualizationEnabled{};

								FRIEND_JSON_CONVERSION_FUNCTIONS(Cell, Size, IsVisualizationEnabled);
							} Cell;

							static constexpr uint32_t MaxBuildSamples = 32;
							uint32_t BuildSamples = 8;

							FRIEND_JSON_CONVERSION_FUNCTIONS(ReGIR, Cell, BuildSamples);
						} ReGIR;

						struct InitialSampling {
							struct LocalLight {
								ReSTIRDI_LocalLightSamplingMode Mode = ReSTIRDI_LocalLightSamplingMode::ReGIR_RIS;

								static constexpr uint32_t MaxSamples = 32;
								uint32_t Samples = 8;

								FRIEND_JSON_CONVERSION_FUNCTIONS(LocalLight, Mode, Samples);
							} LocalLight;

							static constexpr uint32_t MaxBRDFSamples = 8;
							uint32_t BRDFSamples = 1;

							FRIEND_JSON_CONVERSION_FUNCTIONS(InitialSampling, LocalLight, BRDFSamples);
						} InitialSampling;

						struct TemporalResampling {
							ReSTIRDI_TemporalBiasCorrectionMode BiasCorrectionMode = ReSTIRDI_TemporalBiasCorrectionMode::Basic;

							struct BoilingFilter {
								bool IsEnabled = true;

								float Strength = 0.2f;

								FRIEND_JSON_CONVERSION_FUNCTIONS(BoilingFilter, IsEnabled, Strength);
							} BoilingFilter;

							FRIEND_JSON_CONVERSION_FUNCTIONS(TemporalResampling, BiasCorrectionMode, BoilingFilter);
						} TemporalResampling;

						struct SpatialResampling {
							ReSTIRDI_SpatialBiasCorrectionMode BiasCorrectionMode = ReSTIRDI_SpatialBiasCorrectionMode::Basic;

							static constexpr uint32_t MaxSamples = 32;
							uint32_t Samples = 1;

							FRIEND_JSON_CONVERSION_FUNCTIONS(SpatialResampling, BiasCorrectionMode, Samples);
						} SpatialResampling;

						FRIEND_JSON_CONVERSION_FUNCTIONS(ReSTIRDI, IsEnabled, ReGIR, InitialSampling, TemporalResampling, SpatialResampling);
					} ReSTIRDI;

					FRIEND_JSON_CONVERSION_FUNCTIONS(RTXDI, ReSTIRDI);
				} RTXDI;

				struct RTXGI {
					RTXGITechnique Technique = RTXGITechnique::SHARC;

					struct SHARC {
						static constexpr uint32_t MaxDownscaleFactor = 4;
						uint32_t DownscaleFactor = 4;

						static constexpr float MinSceneScale = 5, MaxSceneScale = 100;
						float SceneScale = 50;

						float RoughnessThreshold = 0.4f;

						bool IsHashGridVisualizationEnabled{};

						FRIEND_JSON_CONVERSION_FUNCTIONS(SHARC, DownscaleFactor, SceneScale, RoughnessThreshold, IsHashGridVisualizationEnabled);
					} SHARC;

					FRIEND_JSON_CONVERSION_FUNCTIONS(RTXGI, Technique, SHARC);
				} RTXGI;

				FRIEND_JSON_CONVERSION_FUNCTIONS(Raytracing, IsRussianRouletteEnabled, Bounces, SamplesPerPixel, IsShaderExecutionReorderingEnabled, RTXDI, RTXGI);
			} Raytracing;

			struct PostProcessing {
				struct SuperResolution {
					Upscaler Upscaler = Upscaler::DLSS;

					SuperResolutionMode Mode = SuperResolutionMode::Auto;

					FRIEND_JSON_CONVERSION_FUNCTIONS(SuperResolution, Upscaler, Mode);
				} SuperResolution;

				struct Denoising {
					Denoiser Denoiser = Denoiser::DLSSRayReconstruction;

					bool IsNRDValidationOverlayEnabled{};

					FRIEND_JSON_CONVERSION_FUNCTIONS(Denoising, Denoiser, IsNRDValidationOverlayEnabled);
				} Denoising;

				bool IsDLSSFrameGenerationEnabled = true;

				struct NIS {
					bool IsEnabled{};

					float Sharpness = 0.5f;

					FRIEND_JSON_CONVERSION_FUNCTIONS(NIS, IsEnabled, Sharpness);
				} NIS;

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
						DirectX::ToneMapPostProcess::Operator Operator = DirectX::ToneMapPostProcess::ACESFilmic;

						static constexpr float MinExposure = -10, MaxExposure = 10;
						float Exposure{};

						FRIEND_JSON_CONVERSION_FUNCTIONS(NonHDR, Operator, Exposure);
					} NonHDR;

					FRIEND_JSON_CONVERSION_FUNCTIONS(ToneMapping, HDR, NonHDR);
				} ToneMapping;

				FRIEND_JSON_CONVERSION_FUNCTIONS(PostProcessing, SuperResolution, Denoising, IsDLSSFrameGenerationEnabled, NIS, Bloom, ToneMapping);
			} PostProcessing;

			FRIEND_JSON_CONVERSION_FUNCTIONS(Graphics, WindowMode, Resolution, IsHDREnabled, IsVSyncEnabled, ReflexMode, Camera, Raytracing, PostProcessing);

			void Check() override {
				using namespace std;

				Camera.HorizontalFieldOfView = clamp(Camera.HorizontalFieldOfView, Camera.MinHorizontalFieldOfView, Camera.MaxHorizontalFieldOfView);

				{
					Raytracing.Bounces = min(Raytracing.Bounces, Raytracing.MaxBounces);
					Raytracing.SamplesPerPixel = clamp(Raytracing.SamplesPerPixel, 1u, Raytracing.MaxSamplesPerPixel);

					{
						Raytracing.RTXDI.ReSTIRDI.ReGIR.Cell.Size = clamp(Raytracing.RTXDI.ReSTIRDI.ReGIR.Cell.Size, Raytracing.RTXDI.ReSTIRDI.ReGIR.Cell.MinSize, Raytracing.RTXDI.ReSTIRDI.ReGIR.Cell.MaxSize);
						Raytracing.RTXDI.ReSTIRDI.ReGIR.BuildSamples = clamp(Raytracing.RTXDI.ReSTIRDI.ReGIR.BuildSamples, 1u, Raytracing.RTXDI.ReSTIRDI.ReGIR.MaxBuildSamples);
						Raytracing.RTXDI.ReSTIRDI.InitialSampling.LocalLight.Samples = clamp(Raytracing.RTXDI.ReSTIRDI.InitialSampling.LocalLight.Samples, 1u, Raytracing.RTXDI.ReSTIRDI.InitialSampling.LocalLight.MaxSamples);
						Raytracing.RTXDI.ReSTIRDI.InitialSampling.BRDFSamples = clamp(Raytracing.RTXDI.ReSTIRDI.InitialSampling.BRDFSamples, 1u, Raytracing.RTXDI.ReSTIRDI.InitialSampling.MaxBRDFSamples);
						Raytracing.RTXDI.ReSTIRDI.TemporalResampling.BoilingFilter.Strength = clamp(Raytracing.RTXDI.ReSTIRDI.TemporalResampling.BoilingFilter.Strength, 0.0f, 1.0f);
						Raytracing.RTXDI.ReSTIRDI.SpatialResampling.Samples = clamp(Raytracing.RTXDI.ReSTIRDI.SpatialResampling.Samples, 1u, Raytracing.RTXDI.ReSTIRDI.SpatialResampling.MaxSamples);
					}

					{
						Raytracing.RTXGI.SHARC.DownscaleFactor = clamp(Raytracing.RTXGI.SHARC.DownscaleFactor, 1u, Raytracing.RTXGI.SHARC.MaxDownscaleFactor);
						Raytracing.RTXGI.SHARC.SceneScale = clamp(Raytracing.RTXGI.SHARC.SceneScale, Raytracing.RTXGI.SHARC.MinSceneScale, Raytracing.RTXGI.SHARC.MaxSceneScale);
						Raytracing.RTXGI.SHARC.RoughnessThreshold = clamp(Raytracing.RTXGI.SHARC.RoughnessThreshold, 0.0f, 1.0f);
					}
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

		static auto Load() {
			auto ret = false;
			if (Graphics.Load()) {
				Graphics.Check();
				ret &= true;
			}
			if (UI.Load()) {
				UI.Check();
				ret &= true;
			}
			if (Controls.Load()) {
				Controls.Check();
				ret &= true;
			}
			return ret;
		}

		static auto Save() {
			auto ret = false;
			ret &= Graphics.Save();
			ret &= UI.Save();
			ret &= Controls.Save();
			return ret;
		}

	private:
		inline static const struct _ {
			_() { std::ignore = Load(); }
		} _;
	};
};
