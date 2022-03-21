#pragma once

#include <Windows.h>

#include <set>

namespace DisplayHelpers {
	struct Resolution : SIZE {
		friend bool operator<(const SIZE& lhs, const SIZE& rhs) {
			if (lhs.cx < rhs.cx) return true;
			if (lhs.cx > rhs.cx) return false;
			return lhs.cy < rhs.cy;
		}

		friend bool operator>=(const SIZE& lhs, const SIZE& rhs) { return !(lhs < Resolution(rhs)); }

		friend bool operator>(const SIZE& lhs, const SIZE& rhs) { return Resolution(rhs) < lhs; }

		friend bool operator<=(const SIZE& lhs, const SIZE& rhs) { return !(lhs > Resolution(rhs)); }

		friend bool operator==(const SIZE& lhs, const SIZE& rhs) { return lhs.cx == rhs.cx && lhs.cy == rhs.cy; }

		friend bool operator!=(const SIZE& lhs, const SIZE& rhs) { return !(Resolution(lhs) == rhs); }
	};

	using ResolutionSet = std::set<Resolution>;

	inline BOOL WINAPI GetDisplayResolutions(ResolutionSet& resolutions, LPCWSTR lpDeviceName = nullptr) {
		DEVMODEW devMode;
		devMode.dmSize = sizeof(devMode);
		DWORD i;
		for (i = 0; EnumDisplaySettingsW(lpDeviceName, i, &devMode); i++) {
			resolutions.insert({ static_cast<LONG>(devMode.dmPelsWidth), static_cast<LONG>(devMode.dmPelsHeight) });
		}
		return i != 0;
	}

	inline BOOL WINAPI GetDisplayResolutions(ResolutionSet& resolutions, HMONITOR hMonitor) {
		MONITORINFOEXW monitorInfoEx;
		monitorInfoEx.cbSize = sizeof(monitorInfoEx);
		return GetMonitorInfoW(hMonitor, &monitorInfoEx) && GetDisplayResolutions(resolutions, monitorInfoEx.szDevice);
	}

	inline BOOL WINAPI GetDisplayResolutions(ResolutionSet& resolutions, HWND hWnd) {
		const auto monitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
		return monitor != nullptr && GetDisplayResolutions(resolutions, monitor);
	}

	inline BOOL WINAPI GetDisplayRect(RECT& rect, HMONITOR hMonitor) {
		MONITORINFO monitorInfo;
		monitorInfo.cbSize = sizeof(monitorInfo);
		const auto ret = GetMonitorInfoW(hMonitor, &monitorInfo);
		if (ret) rect = monitorInfo.rcMonitor;
		return ret;
	}

	inline BOOL WINAPI GetDisplayRect(RECT& rect, HWND hWnd) {
		const auto monitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
		return monitor != nullptr && GetDisplayRect(rect, monitor);
	}
}
