#pragma once

#include <Windows.h>

#include <vector>

namespace DisplayHelpers {
	struct Resolution : SIZE {
		friend bool operator<(const SIZE& lhs, const SIZE& rhs) {
			if (lhs.cx < rhs.cx) return true;
			if (lhs.cx > rhs.cx) return false;
			return lhs.cy < rhs.cy;
		}

		friend bool operator>=(const SIZE& lhs, const SIZE& rhs) { return !(lhs < Resolution{ rhs.cx, rhs.cy }); }

		friend bool operator>(const SIZE& lhs, const SIZE& rhs) { return Resolution{ rhs.cx, rhs.cy } < lhs; }

		friend bool operator<=(const SIZE& lhs, const SIZE& rhs) { return !(lhs > Resolution{ rhs.cx, rhs.cy }); }

		friend bool operator==(const SIZE& lhs, const SIZE& rhs) { return lhs.cx == rhs.cx && lhs.cy == rhs.cy; }

		friend bool operator!=(const SIZE& lhs, const SIZE& rhs) { return !(Resolution{ lhs.cx, lhs.cy } == rhs); }
	};

	inline BOOL WINAPI GetDisplayResolutions(std::vector<Resolution>& resolutions, LPCWSTR lpszDeviceName = nullptr) {
		DEVMODEW devMode;
		devMode.dmSize = sizeof(devMode);
		for (DWORD i = 0; EnumDisplaySettingsW(lpszDeviceName, i++, &devMode);) {
			const auto& iteratorEnd = resolutions.cend();
			const Resolution resolution{ static_cast<LONG>(devMode.dmPelsWidth), static_cast<LONG>(devMode.dmPelsHeight) };
			if (std::find(resolutions.cbegin(), iteratorEnd, resolution) == iteratorEnd) resolutions.push_back(resolution);
		}

		const auto lastError = GetLastError();
		return lastError == ERROR_SUCCESS || GetLastError() == ERROR_MOD_NOT_FOUND;
	}
}
