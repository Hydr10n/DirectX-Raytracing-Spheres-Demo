#pragma once

#include "nlohmann/json.hpp"

#include <fstream>

#include <Windows.h>

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

struct MyAppData {
	struct Settings {
		inline static const std::filesystem::path DirectoryPath = std::filesystem::path(*__wargv).replace_filename(L"Settings");

		inline static struct Graphics {
			inline static const std::filesystem::path FilePath = DirectoryPath / L"Graphics.json";

			WindowHelpers::WindowMode WindowMode{};

			DisplayHelpers::Resolution Resolution{};

			struct Raytracing {
				UINT MaxTraceRecursionDepth = 8, SamplesPerPixel = 2;

				NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT_ORDERED_F(Raytracing, MaxTraceRecursionDepth, SamplesPerPixel);
			} Raytracing;

			struct TemporalAntiAliasing : DirectX::PostProcess::TemporalAntiAliasing::Constant {
				bool IsEnabled = true;

				NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT_ORDERED_F(TemporalAntiAliasing, IsEnabled, Alpha, ColorBoxSigma);
			} TemporalAntiAliasing;

			NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT_ORDERED_F(Graphics, WindowMode, Resolution, Raytracing, TemporalAntiAliasing);
		} Graphics;

	private:
		inline static const struct _ { _() { create_directories(DirectoryPath); } } ms_settings;
	};

	template <typename T>
	static bool Load(T& data) {
		try {
			std::ifstream file(T::FilePath);
			ordered_json_f json;
			file >> json;
			data = json;
			return true;
		}
		catch (...) { return false; }
	}

	template <typename T>
	static bool Save(const T& data) {
		try {
			std::ofstream file(T::FilePath, std::ios_base::trunc);
			file << std::setw(4) << ordered_json_f(data);
			return true;
		}
		catch (...) { return false; }
	}
};
