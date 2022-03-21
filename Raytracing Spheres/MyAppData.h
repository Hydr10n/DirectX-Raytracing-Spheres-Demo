#pragma once

#include "AppData.h"

#include <filesystem>

struct MyAppData {
	struct Settings {
		struct Sections { static constexpr LPCWSTR Graphics = L"Graphics"; };

		struct Keys {
			struct WindowMode {
				static constexpr LPCWSTR Section = Sections::Graphics;
				static constexpr auto ToString() { return L"WindowMode"; }
			};

			struct Resolution {
				static constexpr LPCWSTR Section = Sections::Graphics;
				static constexpr auto ToStrings() { return std::pair(L"ResolutionWidth", L"ResolutionHeight"); }
			};

			struct AntiAliasingSampleCount {
				static constexpr LPCWSTR Section = Sections::Graphics;
				static constexpr auto ToString() { return L"AntiAliasingSampleCount"; }
			};
		};

		template <class Key, class Data>
		static BOOL Save(const Data& data) {
			if constexpr (std::is_same<Key, Keys::WindowMode>()) {
				return m_appData.Save(Key::Section, Key::ToString(), reinterpret_cast<const UINT&>(data));
			}

			if constexpr (std::is_same<Key, Keys::Resolution>()) {
				constexpr auto Keys = Key::ToStrings();
				const auto& size = reinterpret_cast<const SIZE&>(data);
				return m_appData.Save(Key::Section, Keys.first, size.cx)
					&& m_appData.Save(Key::Section, Keys.second, size.cy);
			}

			if constexpr (std::is_same<Key, Keys::AntiAliasingSampleCount>()) {
				return m_appData.Save(Key::Section, Key::ToString(), data);
			}

			throw;
		}

		template <class Key, class Data>
		static BOOL Load(Data& data) {
			if constexpr (std::is_same<Key, Keys::WindowMode>()) {
				return m_appData.Load(Key::Section, Key::ToString(), reinterpret_cast<UINT&>(data));
			}

			if constexpr (std::is_same<Key, Keys::Resolution>()) {
				constexpr auto Keys = Key::ToStrings();
				auto& size = reinterpret_cast<SIZE&>(data);
				return m_appData.Load(Key::Section, Keys.first, size.cx)
					&& m_appData.Load(Key::Section, Keys.second, size.cy);
			}

			if constexpr (std::is_same<Key, Keys::AntiAliasingSampleCount>()) {
				return m_appData.Load(Key::Section, Key::ToString(), data);
			}

			throw;
		}

	private:
		inline static const Hydr10n::Data::AppData m_appData = std::filesystem::path(*__wargv).replace_filename("Settings.ini").c_str();
	};
};
