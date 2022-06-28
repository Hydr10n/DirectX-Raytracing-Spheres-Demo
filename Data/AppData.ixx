/*
 * Header File: AppData.ixx
 * Last Update: 2022/06/28
 *
 * Copyright (C) Hydr10n@GitHub. All Rights Reserved.
 */

module;
#include <Windows.h>

#include <sstream>

export module Hydr10n.Data.AppData;

namespace Hydr10n::Data {
	export struct AppData {
		const std::wstring Path;

		AppData(LPCWSTR lpPath) : Path(lpPath) {}

		[[nodiscard]] BOOL Load(LPCWSTR lpSection, LPCWSTR lpKey, LPWSTR data, DWORD nSize) const {
			GetPrivateProfileStringW(lpSection, lpKey, nullptr, data, nSize, Path.c_str());
			return GetLastError() == ERROR_SUCCESS;
		}

		template <typename T> requires std::integral<T> || std::floating_point<T>
		[[nodiscard]] BOOL Load(LPCWSTR lpSection, LPCWSTR lpKey, T & data) const {
			WCHAR buffer[1025];
			if (Load(lpSection, lpKey, buffer, ARRAYSIZE(buffer))) {
				WCHAR ch;
				std::wistringstream istringstream(buffer);
				if (istringstream >> data && !(istringstream >> ch)) return TRUE;
				SetLastError(ERROR_INVALID_DATA);
			}
			return FALSE;
		}

		[[nodiscard]] BOOL Save(LPCWSTR lpSection, LPCWSTR lpKey, LPCWSTR data) const {
			return WritePrivateProfileStringW(lpSection, lpKey, data, Path.c_str());
		}

		template <typename T>  requires std::integral<T> || std::floating_point<T>
		[[nodiscard]] BOOL Save(LPCWSTR lpSection, LPCWSTR lpKey, const T & data) const {
			return Save(lpSection, lpKey, std::to_wstring(data).c_str());
		}
	};
}
