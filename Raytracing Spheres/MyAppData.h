#pragma once

#include "nlohmann/json.hpp"

#include <Windows.h>

#include <fstream>

import DirectX.PostProcess.TemporalAntiAliasing;
import DisplayHelpers;
import WindowHelpers;

using ordered_json_f = nlohmann::basic_json<nlohmann::ordered_map, std::vector, std::string, bool, int64_t, uint64_t, float>;

#define NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT_ORDERED_F(Type, ...) \
	friend void to_json(ordered_json_f& nlohmann_json_j, const Type& nlohmann_json_t) { NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(NLOHMANN_JSON_TO, __VA_ARGS__)) } \
	friend void from_json(const ordered_json_f& nlohmann_json_j, Type& nlohmann_json_t) { Type nlohmann_json_default_obj; NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(NLOHMANN_JSON_FROM_WITH_DEFAULT, __VA_ARGS__)) }

inline void to_json(ordered_json_f& json, const SIZE& data) { json = { { "Width", data.cx }, { "Height", data.cy } }; }
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
				float VerticalFieldOfView = 45;

				struct DepthOfField {
					bool IsEnabled = true;

					float FocusDistance = std::numeric_limits<float>::infinity(), ApertureRadius = 5;

					NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT_ORDERED_F(DepthOfField, IsEnabled, FocusDistance, ApertureRadius);
				} DepthOfField;

				NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT_ORDERED_F(Camera, VerticalFieldOfView, DepthOfField);
			} Camera;

			struct Raytracing {
				UINT MaxTraceRecursionDepth = 8, SamplesPerPixel = 1;

				NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT_ORDERED_F(Raytracing, MaxTraceRecursionDepth, SamplesPerPixel);
			} Raytracing;

			struct PostProcessing {
				struct TemporalAntiAliasing : DirectX::PostProcess::TemporalAntiAliasing::Constant {
					bool IsEnabled = true;

					NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT_ORDERED_F(TemporalAntiAliasing, IsEnabled, Alpha, ColorBoxSigma);
				} TemporalAntiAliasing;

				NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT_ORDERED_F(PostProcessing, TemporalAntiAliasing);
			} PostProcessing;

			NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT_ORDERED_F(Graphics, WindowMode, Resolution, IsVSyncEnabled, Camera, Raytracing, PostProcessing);
		} Graphics;

		inline static struct UI : Data<UI> {
			inline static const std::filesystem::path FilePath = DirectoryPath / L"UI.json";

			struct Menu {
				bool IsOpenOnStartup = true;

				float BackgroundOpacity = 0.5f;

				NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT_ORDERED_F(Menu, IsOpenOnStartup, BackgroundOpacity);
			} Menu;

			NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT_ORDERED_F(UI, Menu);
		} UI;

		inline static struct Controls : Data<Controls> {
			inline static const std::filesystem::path FilePath = DirectoryPath / L"Controls.json";

			struct Camera {
				struct Speed {
					float Translation = 10, Rotation = 0.5f;

					NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT_ORDERED_F(Speed, Translation, Rotation);
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
