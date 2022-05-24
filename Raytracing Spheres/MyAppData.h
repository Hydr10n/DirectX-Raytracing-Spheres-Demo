#pragma once

#include "AppData.h"

#include <filesystem>

#define MAKESECTION(SectionName, Members) struct SectionName { static constexpr LPCWSTR Name = L#SectionName; Members };

#define MAKEKEY(KeyName) struct KeyName { static constexpr LPCWSTR Section = Name, Key = L#KeyName; };

struct MyAppData {
	struct Settings {
		struct Graphics {
			static constexpr LPCWSTR Name = L"";

			MAKEKEY(WindowMode);

			MAKESECTION(Resolution, MAKEKEY(Width) MAKEKEY(Height));

			MAKESECTION(Raytracing, MAKEKEY(SamplesPerPixel));

			MAKESECTION(TemporalAntiAliasing, MAKEKEY(IsEnabled) MAKEKEY(Alpha) MAKEKEY(ColorBoxSigma));

			template <class Key, class Data>
			static BOOL Save(const Data& data) {
				if constexpr (std::is_same<Key, WindowMode>()) {
					return m_appData.Save(Key::Section, Key::Key, reinterpret_cast<const UINT&>(data));
				}

				if constexpr (std::is_same<Key, Resolution>()) {
					const auto& size = reinterpret_cast<const SIZE&>(data);
					return m_appData.Save(Key::Width::Section, Key::Width::Key, size.cx)
						&& m_appData.Save(Key::Height::Section, Key::Height::Key, size.cy);
				}

				if constexpr (
					std::is_same<Key, Raytracing::SamplesPerPixel>()
					|| std::is_same<Key, TemporalAntiAliasing::IsEnabled>()
					|| std::is_same<Key, TemporalAntiAliasing::Alpha>()
					|| std::is_same<Key, TemporalAntiAliasing::ColorBoxSigma>()
					) {
					return m_appData.Save(Key::Section, Key::Key, data);
				}

				throw;
			}

			template <class Key, class Data>
			static BOOL Load(Data& data) {
				if constexpr (std::is_same<Key, WindowMode>()) {
					return m_appData.Load(Key::Section, Key::Key, reinterpret_cast<UINT&>(data));
				}

				if constexpr (std::is_same<Key, Resolution>()) {
					auto& size = reinterpret_cast<SIZE&>(data);
					return m_appData.Load(Key::Width::Section, Key::Width::Key, size.cx)
						&& m_appData.Load(Key::Height::Section, Key::Height::Key, size.cy);
				}

				if constexpr (
					std::is_same<Key, Raytracing::SamplesPerPixel>()
					|| std::is_same<Key, TemporalAntiAliasing::IsEnabled>()
					|| std::is_same<Key, TemporalAntiAliasing::Alpha>()
					|| std::is_same<Key, TemporalAntiAliasing::ColorBoxSigma>()
					) {
					return m_appData.Load(Key::Section, Key::Key, data);
				}

				throw;
			}

		private:
			inline static const Hydr10n::Data::AppData m_appData = std::filesystem::path(*__wargv).replace_filename("GraphicsSettings.ini").c_str();
		};
	};
};
