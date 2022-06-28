#pragma once

#include <filesystem>

import WindowHelpers;

import Hydr10n.Data.AppData;

#define MAKESECTION(Name, Members) class Name { static constexpr LPCWSTR SectionName = L#Name; public: Members };

#define MAKEKEY(KeyName) struct KeyName { static constexpr LPCWSTR Section = SectionName, Key = L#KeyName; };

struct MyAppData {
	struct Settings {
		class Graphics {
			static constexpr LPCWSTR SectionName = L"";

		public:
			MAKEKEY(WindowMode);

			MAKESECTION(Resolution, MAKEKEY(Width) MAKEKEY(Height));

			MAKESECTION(Raytracing, MAKEKEY(SamplesPerPixel));

			MAKESECTION(TemporalAntiAliasing, MAKEKEY(IsEnabled) MAKEKEY(Alpha) MAKEKEY(ColorBoxSigma));

			template <std::same_as<WindowMode> Key>
			[[nodiscard]] static BOOL Load(WindowHelpers::WindowMode& data) {
				return m_appData.Load(WindowMode::Section, WindowMode::Key, reinterpret_cast<UINT&>(data));
			}

			template <std::same_as<WindowMode> Key>
			static BOOL Save(const WindowHelpers::WindowMode& data) {
				return m_appData.Save(WindowMode::Section, WindowMode::Key, reinterpret_cast<const UINT&>(data));
			}

			template <std::same_as<Resolution> Key>
			[[nodiscard]] static BOOL Load(SIZE& data) {
				return m_appData.Load(Resolution::Width::Section, Resolution::Width::Key, data.cx)
					&& m_appData.Load(Resolution::Height::Section, Resolution::Height::Key, data.cy);
			}

			template <std::same_as<Resolution> Key>
			static BOOL Save(const SIZE& data) {
				return m_appData.Save(Resolution::Width::Section, Resolution::Width::Key, data.cx)
					&& m_appData.Save(Resolution::Height::Section, Resolution::Height::Key, data.cy);
			}

			template <typename Key, typename Data> requires
				(std::same_as<Key, Raytracing::SamplesPerPixel>
					|| std::same_as<Key, TemporalAntiAliasing::IsEnabled>
					|| std::same_as<Key, TemporalAntiAliasing::Alpha>
					|| std::same_as<Key, TemporalAntiAliasing::ColorBoxSigma>)
				&& (std::integral<Data> || std::floating_point<Data>)
				[[nodiscard]] static BOOL Load(Data& data) { return m_appData.Load(Key::Section, Key::Key, data); }

			template <typename Key, typename Data> requires
				(std::same_as<Key, Raytracing::SamplesPerPixel>
					|| std::same_as<Key, TemporalAntiAliasing::IsEnabled>
					|| std::same_as<Key, TemporalAntiAliasing::Alpha>
					|| std::same_as<Key, TemporalAntiAliasing::ColorBoxSigma>)
				&& (std::integral<Data> || std::floating_point<Data>)
				static BOOL Save(const Data& data) { return m_appData.Save(Key::Section, Key::Key, data); }

		private:
			inline static const Hydr10n::Data::AppData m_appData = std::filesystem::path(*__wargv).replace_filename(L"GraphicsSettings.ini").c_str();
		};
	};
};
