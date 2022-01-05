/*
 * Header File: AppData.h
 * Last Update: 2022/01/04
 *
 * Copyright (C) Hydr10n@GitHub. All Rights Reserved.
 */

#pragma once

#include <Windows.h>

#include <sstream>

namespace Hydr10n::Data {
	struct AppData {
		const std::wstring Path;

		AppData(LPCWSTR lpPath) : Path(lpPath) {}

		BOOL Save(LPCWSTR lpSection, LPCWSTR lpKey, LPCWSTR data) const { return WritePrivateProfileStringW(lpSection, lpKey, data, Path.c_str()); }

		template <class T>
		BOOL Save(LPCWSTR lpSection, LPCWSTR lpKey, const T& data) const { return Save(lpSection, lpKey, std::to_wstring(data).c_str()); }

		BOOL Load(LPCWSTR lpSection, LPCWSTR lpKey, LPWSTR data, DWORD nSize) const { return GetPrivateProfileStringW(lpSection, lpKey, nullptr, data, nSize, Path.c_str()), GetLastError() == ERROR_SUCCESS; }

		template <class T>
		BOOL Load(LPCWSTR lpSection, LPCWSTR lpKey, T& data) const {
			WCHAR buffer[1025];
			if (Load(lpSection, lpKey, buffer, ARRAYSIZE(buffer))) {
				WCHAR ch;
				std::wistringstream istringstream(buffer);
				if (istringstream >> data && !(istringstream >> ch)) return TRUE;
				SetLastError(ERROR_INVALID_DATA);
			}
			return FALSE;
		}
	};
}
