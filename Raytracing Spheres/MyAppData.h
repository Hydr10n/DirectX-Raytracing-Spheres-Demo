#pragma once

#include "JsonHelpers.h"

#include <Windows.h>

#include <fstream>

import DisplayHelpers;
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
				bool IsJitterEnabled = true;

				float VerticalFieldOfView = 45;

				FRIEND_JSON_CONVERSION_FUNCTIONS(Camera, IsJitterEnabled, VerticalFieldOfView);
			} Camera;

			struct Raytracing {
				bool IsRussianRouletteEnabled = true;

				UINT MaxTraceRecursionDepth = 8, SamplesPerPixel = 1;

				FRIEND_JSON_CONVERSION_FUNCTIONS(Raytracing, IsRussianRouletteEnabled, MaxTraceRecursionDepth, SamplesPerPixel);
			} Raytracing;

			struct PostProcessing {
				struct RaytracingDenoising {
					bool IsEnabled = true, IsValidationLayerEnabled{};

					float SplitScreen{};

					FRIEND_JSON_CONVERSION_FUNCTIONS(RaytracingDenoising, IsEnabled, IsValidationLayerEnabled, SplitScreen);
				} RaytracingDenoising;

				bool IsTemporalAntiAliasingEnabled = true;

				struct Bloom {
					bool IsEnabled = true;

					float Threshold = 0.5f, BlurSize = 5;

					FRIEND_JSON_CONVERSION_FUNCTIONS(Bloom, IsEnabled, Threshold, BlurSize);
				} Bloom;

				FRIEND_JSON_CONVERSION_FUNCTIONS(PostProcessing, RaytracingDenoising, IsTemporalAntiAliasingEnabled, Bloom);
			} PostProcessing;

			FRIEND_JSON_CONVERSION_FUNCTIONS(Graphics, WindowMode, Resolution, IsVSyncEnabled, Camera, Raytracing, PostProcessing);
		} Graphics;

		inline static struct UI : Data<UI> {
			inline static const std::filesystem::path FilePath = DirectoryPath / L"UI.json";

			bool ShowOnStartup = true;

			float WindowOpacity = 0.5f;

			FRIEND_JSON_CONVERSION_FUNCTIONS(UI, ShowOnStartup, WindowOpacity);
		} UI;

		inline static struct Controls : Data<Controls> {
			inline static const std::filesystem::path FilePath = DirectoryPath / L"Controls.json";

			struct Camera {
				struct Speed {
					float Movement = 10, Rotation = 0.5f;

					FRIEND_JSON_CONVERSION_FUNCTIONS(Speed, Movement, Rotation);
				} Speed;

				FRIEND_JSON_CONVERSION_FUNCTIONS(Camera, Speed);
			} Camera;

			FRIEND_JSON_CONVERSION_FUNCTIONS(Controls, Camera);
		} Controls;

	private:
		inline static const struct _ {
			_() {
				std::ignore = Graphics.Load();
				std::ignore = UI.Load();
				std::ignore = Controls.Load();
			}
		} _;
	};
};
