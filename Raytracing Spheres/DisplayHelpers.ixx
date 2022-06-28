module;
#include <Windows.h>

#include <set>

export module DisplayHelpers;

using namespace std;

export namespace DisplayHelpers {
	struct Resolution : SIZE {
		[[nodiscard]] bool IsPortrait() const { return cx < cy; }

		[[nodiscard]] friend bool operator<(const SIZE& lhs, const SIZE& rhs) {
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

	using ResolutionSet = set<Resolution>;

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

	[[nodiscard]] inline BOOL WINAPI GetDisplayResolution(Resolution& resolution, HMONITOR hMonitor) {
		RECT rect;
		const auto ret = GetDisplayRect(rect, hMonitor);
		if (ret) resolution = { rect.right - rect.left, rect.bottom - rect.top };
		return ret;
	}

	[[nodiscard]] inline BOOL WINAPI GetDisplayResolution(Resolution& resolution, HWND hWnd) {
		const auto monitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
		return monitor != nullptr && GetDisplayResolution(resolution, monitor);
	}

	[[nodiscard]] inline BOOL WINAPI GetDisplayResolution(DWORD index, Resolution& resolution, LPCWSTR lpDeviceName = nullptr) {
		DEVMODEW devMode;
		devMode.dmSize = sizeof(devMode);
		const auto ret = EnumDisplaySettingsW(lpDeviceName, index, &devMode);
		if (ret) resolution = { static_cast<LONG>(devMode.dmPelsWidth), static_cast<LONG>(devMode.dmPelsHeight) };
		return ret;
	}

	[[nodiscard]] inline BOOL WINAPI GetDisplayResolution(DWORD index, Resolution& resolution, HMONITOR hMonitor) {
		MONITORINFOEXW monitorInfoEx;
		monitorInfoEx.cbSize = sizeof(monitorInfoEx);
		return GetMonitorInfoW(hMonitor, &monitorInfoEx) && GetDisplayResolution(index, resolution, monitorInfoEx.szDevice);
	}

	[[nodiscard]] inline BOOL WINAPI GetDisplayResolution(DWORD index, Resolution& resolution, HWND hWnd) {
		const auto monitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
		return monitor != nullptr && GetDisplayResolution(index, resolution, monitor);
	}

	[[nodiscard]] inline BOOL WINAPI GetDisplayResolutions(ResolutionSet& resolutions, LPCWSTR lpDeviceName = nullptr) {
		DWORD i;
		Resolution resolution;
		for (i = 0; GetDisplayResolution(i, resolution, lpDeviceName); i++) resolutions.insert(resolution);
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
}
