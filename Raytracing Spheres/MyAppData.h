#pragma once

#include "AppData.h"

#include "WindowHelpers.h"

struct MyAppData {
	class Settings {
	public:
		struct Keys {
			struct WindowMode;
			struct Resolution;
			struct AntiAliasingSampleCount;
		};

		template <class Key, class Data>
		static BOOL Save(const Data& data) {
			if constexpr (std::is_same<Key, Keys::WindowMode>()) {
				return m_appData.Save(StringSections.Graphics, StringKeys.WindowMode, reinterpret_cast<const UINT&>(data));
			}

			if constexpr (std::is_same<Key, Keys::Resolution>()) {
				return m_appData.Save(StringSections.Graphics, StringKeys.ResolutionWidth, data.cx)
					&& m_appData.Save(StringSections.Graphics, StringKeys.ResolutionHeight, data.cy);
			}

			if constexpr (std::is_same<Key, Keys::AntiAliasingSampleCount>()) {
				return m_appData.Save(StringSections.Graphics, StringKeys.AntiAliasingSampleCount, data);
			}

			throw;
		}

		template <class Key, class Data>
		static BOOL Load(Data& data) {
			if constexpr (std::is_same<Key, Keys::WindowMode>()) {
				return m_appData.Load(StringSections.Graphics, StringKeys.WindowMode, reinterpret_cast<UINT&>(data));
			}

			if constexpr (std::is_same<Key, Keys::Resolution>()) {
				return m_appData.Load(StringSections.Graphics, StringKeys.ResolutionWidth, data.cx)
					&& m_appData.Load(StringSections.Graphics, StringKeys.ResolutionHeight, data.cy);
			}

			if constexpr (std::is_same<Key, Keys::AntiAliasingSampleCount>()) {
				return m_appData.Load(StringSections.Graphics, StringKeys.AntiAliasingSampleCount, data);
			}

			throw;
		}

	private:
		static constexpr struct { LPCWSTR Graphics = L"Graphics"; } StringSections{};

		static constexpr struct {
			LPCWSTR WindowMode = L"WindowMode";
			LPCWSTR ResolutionWidth = L"ResolutionWidth", ResolutionHeight = L"ResolutionHeight";
			LPCWSTR AntiAliasingSampleCount = L"AntiAliasingSampleCount";
		} StringKeys{};

		static const Hydr10n::Data::AppData m_appData;
	};
};
