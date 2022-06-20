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

		[[nodiscard]] friend bool operator>=(const SIZE& lhs, const SIZE& rhs) { return !(lhs < Resolution(rhs)); }

		[[nodiscard]] friend bool operator>(const SIZE& lhs, const SIZE& rhs) { return Resolution(rhs) < lhs; }

		[[nodiscard]] friend bool operator<=(const SIZE& lhs, const SIZE& rhs) { return !(lhs > Resolution(rhs)); }

		[[nodiscard]] friend bool operator==(const SIZE& lhs, const SIZE& rhs) { return lhs.cx == rhs.cx && lhs.cy == rhs.cy; }

		[[nodiscard]] friend bool operator!=(const SIZE& lhs, const SIZE& rhs) { return !(Resolution(lhs) == rhs); }
	};

	using ResolutionSet = std::set<Resolution>;

	[[nodiscard]] inline BOOL WINAPI GetDisplayResolutions(ResolutionSet& resolutions, LPCWSTR lpDeviceName = nullptr) {
		DEVMODEW devMode;
		devMode.dmSize = sizeof(devMode);
		DWORD i;
		for (i = 0; EnumDisplaySettingsW(lpDeviceName, i, &devMode); i++) {
			resolutions.insert({ static_cast<LONG>(devMode.dmPelsWidth), static_cast<LONG>(devMode.dmPelsHeight) });
		}
		return i != 0;
	}

	[[nodiscard]] inline BOOL WINAPI GetDisplayResolutions(ResolutionSet& resolutions, HMONITOR hMonitor) {
		MONITORINFOEXW monitorInfoEx;
		monitorInfoEx.cbSize = sizeof(monitorInfoEx);
		return GetMonitorInfoW(hMonitor, &monitorInfoEx) && GetDisplayResolutions(resolutions, monitorInfoEx.szDevice);
	}

	[[nodiscard]] inline BOOL WINAPI GetDisplayResolutions(ResolutionSet& resolutions, HWND hWnd) {
		const auto monitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
		return monitor != nullptr && GetDisplayResolutions(resolutions, monitor);
	}

	[[nodiscard]] inline BOOL WINAPI GetDisplayRect(RECT& rect, HMONITOR hMonitor) {
		MONITORINFO monitorInfo;
		monitorInfo.cbSize = sizeof(monitorInfo);
		const auto ret = GetMonitorInfoW(hMonitor, &monitorInfo);
		if (ret) rect = monitorInfo.rcMonitor;
		return ret;
	}

	[[nodiscard]] inline BOOL WINAPI GetDisplayRect(RECT& rect, HWND hWnd) {
		const auto monitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
		return monitor != nullptr && GetDisplayRect(rect, monitor);
	}
}
