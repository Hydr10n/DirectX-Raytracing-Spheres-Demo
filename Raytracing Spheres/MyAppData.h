#pragma once

#include "nlohmann/json.hpp"

#include <Windows.h>

#include <fstream>

import DisplayHelpers;
import WindowHelpers;

using ordered_json_f = nlohmann::basic_json<nlohmann::ordered_map, std::vector, std::string, bool, int64_t, uint64_t, float>;

#define NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT_ORDERED_F(Type, ...) \
	friend void to_json(ordered_json_f& nlohmann_json_j, const Type& nlohmann_json_t) { NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(NLOHMANN_JSON_TO, __VA_ARGS__)) } \
	friend void from_json(const ordered_json_f& nlohmann_json_j, Type& nlohmann_json_t) { Type nlohmann_json_default_obj; NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(NLOHMANN_JSON_FROM_WITH_DEFAULT, __VA_ARGS__)) }

inline void to_json(ordered_json_f& json, SIZE data) { json = { { "Width", data.cx }, { "Height", data.cy } }; }
inline void from_json(const ordered_json_f& json, SIZE& data) { data = { json.value("Width", 0), json.value("Height", 0) }; }

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
				std::ifstream file(T::FilePath);
				ordered_json_f json;
				file >> json;
				*reinterpret_cast<T*>(this) = json;
				return true;
			}
			catch (...) { return false; }
		}

		bool Save() const {
			try {
				std::ofstream file(T::FilePath, std::ios_base::trunc);
				file << std::setw(4) << ordered_json_f(*reinterpret_cast<const T*>(this));
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

				NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT_ORDERED_F(Camera, IsJitterEnabled, VerticalFieldOfView);
			} Camera;

			struct Raytracing {
				bool IsRussianRouletteEnabled = true;

				UINT MaxTraceRecursionDepth = 8, SamplesPerPixel = 1;

				NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT_ORDERED_F(Raytracing, IsRussianRouletteEnabled, MaxTraceRecursionDepth, SamplesPerPixel);
			} Raytracing;

			struct PostProcessing {
				struct RaytracingDenoising {
					bool IsEnabled = true, IsValidationLayerEnabled{};

					float SplitScreen{};

					NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT_ORDERED_F(RaytracingDenoising, IsEnabled, IsValidationLayerEnabled, SplitScreen);
				} RaytracingDenoising;

				bool IsTemporalAntiAliasingEnabled = true;

				struct Bloom {
					bool IsEnabled = true;

					float Threshold = 0.5f, BlurSize = 5;

					NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT_ORDERED_F(Bloom, IsEnabled, Threshold, BlurSize);
				} Bloom;

				NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT_ORDERED_F(PostProcessing, RaytracingDenoising, IsTemporalAntiAliasingEnabled, Bloom);
			} PostProcessing;

			NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT_ORDERED_F(Graphics, WindowMode, Resolution, IsVSyncEnabled, Camera, Raytracing, PostProcessing);
		} Graphics;

		inline static struct UI : Data<UI> {
			inline static const std::filesystem::path FilePath = DirectoryPath / L"UI.json";

			bool ShowOnStartup = true;

			float WindowOpacity = 0.5f;

			NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT_ORDERED_F(UI, ShowOnStartup, WindowOpacity);
		} UI;

		inline static struct Controls : Data<Controls> {
			inline static const std::filesystem::path FilePath = DirectoryPath / L"Controls.json";

			struct Camera {
				struct Speed {
					float Movement = 10, Rotation = 0.5f;

					NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT_ORDERED_F(Speed, Movement, Rotation);
				} Speed;

				NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT_ORDERED_F(Camera, Speed);
			} Camera;

			NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT_ORDERED_F(Controls, Camera);
		} Controls;

	private:
		inline static const struct _ {
			_() {
				create_directories(DirectoryPath);

				std::ignore = Graphics.Load();
				std::ignore = UI.Load();
				std::ignore = Controls.Load();
			}
		} _;
	};
};
