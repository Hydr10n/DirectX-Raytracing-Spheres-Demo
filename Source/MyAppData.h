#pragma once

#include "JsonHelpers.h"

#include "directxtk12/PostProcess.h"

#include <Windows.h>

#include <fstream>

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

			bool IsVSyncEnabled = true;

			struct Camera {
				static constexpr float MinHorizontalFieldOfView = 30, MaxHorizontalFieldOfView = 120;

				bool IsJitterEnabled = true;

				float HorizontalFieldOfView = 90;

				FRIEND_JSON_CONVERSION_FUNCTIONS(Camera, IsJitterEnabled, HorizontalFieldOfView);
			} Camera;

			struct Raytracing {
				static constexpr UINT MaxMaxNumberOfBounces = 64, MaxSamplesPerPixel = 16;

				bool IsRussianRouletteEnabled = false;

				UINT MaxNumberOfBounces = 8, SamplesPerPixel = 1;

				FRIEND_JSON_CONVERSION_FUNCTIONS(Raytracing, IsRussianRouletteEnabled, MaxNumberOfBounces, SamplesPerPixel);
			} Raytracing;

			struct PostProcessing {
				struct NRD {
					NRDDenoiser Denoiser = NRDDenoiser::ReLAX;

					bool IsValidationOverlayEnabled{};

					float SplitScreen{};

					FRIEND_JSON_CONVERSION_FUNCTIONS(NRD, Denoiser, IsValidationOverlayEnabled, SplitScreen);
				} NRD;

				struct SuperResolution {
					Upscaler Upscaler = Upscaler::DLSS;

					SuperResolutionMode Mode = SuperResolutionMode::Auto;

					FRIEND_JSON_CONVERSION_FUNCTIONS(SuperResolution, Upscaler, Mode);
				} SuperResolution;

				struct NIS {
					bool IsEnabled{};

					float Sharpness = 0.5f;

					FRIEND_JSON_CONVERSION_FUNCTIONS(NIS, IsEnabled, Sharpness);
				} NIS;

				bool IsChromaticAberrationEnabled = true;

				struct Bloom {
					static constexpr float MaxBlurSize = 5;

					bool IsEnabled = true;

					float Threshold = 0.5f, BlurSize = 5;

					FRIEND_JSON_CONVERSION_FUNCTIONS(Bloom, IsEnabled, Threshold, BlurSize);
				} Bloom;

				struct ToneMapping {
					static constexpr float MinExposure = -5, MaxExposure = 5;

					DirectX::ToneMapPostProcess::Operator Operator = DirectX::ToneMapPostProcess::ACESFilmic;

					float Exposure{};

					FRIEND_JSON_CONVERSION_FUNCTIONS(ToneMapping, Operator, Exposure);
				} ToneMapping;

				FRIEND_JSON_CONVERSION_FUNCTIONS(PostProcessing, NRD, SuperResolution, NIS, IsChromaticAberrationEnabled, Bloom, ToneMapping);
			} PostProcessing;

			FRIEND_JSON_CONVERSION_FUNCTIONS(Graphics, WindowMode, Resolution, IsVSyncEnabled, Camera, Raytracing, PostProcessing);

			void Check() override {
				using namespace std;

				Camera.HorizontalFieldOfView = clamp(Camera.HorizontalFieldOfView, Camera.MinHorizontalFieldOfView, Camera.MaxHorizontalFieldOfView);

				{
					Raytracing.MaxNumberOfBounces = clamp(Raytracing.MaxNumberOfBounces, 1u, Raytracing.MaxMaxNumberOfBounces);
					Raytracing.SamplesPerPixel = clamp(Raytracing.SamplesPerPixel, 1u, Raytracing.MaxSamplesPerPixel);
				}

				{
					PostProcessing.NRD.SplitScreen = clamp(PostProcessing.NRD.SplitScreen, 0.0f, 1.0f);

					PostProcessing.NIS.Sharpness = clamp(PostProcessing.NIS.Sharpness, 0.0f, 1.0f);

					{
						PostProcessing.Bloom.Threshold = clamp(PostProcessing.Bloom.Threshold, 0.0f, 1.0f);
						PostProcessing.Bloom.BlurSize = clamp(PostProcessing.Bloom.BlurSize, 1.0f, PostProcessing.Bloom.MaxBlurSize);
					}

					{
						using namespace DirectX;

						PostProcessing.ToneMapping.Operator = clamp(PostProcessing.ToneMapping.Operator, ToneMapPostProcess::None, static_cast<ToneMapPostProcess::Operator>(ToneMapPostProcess::Operator_Max - 1));
						PostProcessing.ToneMapping.Exposure = clamp(PostProcessing.ToneMapping.Exposure, PostProcessing.ToneMapping.MinExposure, PostProcessing.ToneMapping.MaxExposure);
					}
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
