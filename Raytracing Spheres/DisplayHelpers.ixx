module;
#include <Windows.h>

#include <set>

export module DisplayHelpers;

using namespace std;

export namespace DisplayHelpers {
	struct Resolution : SIZE {
		[[nodiscard]] bool IsPortrait() const { return cx < cy; }

		[[nodiscard]] bool operator<(const SIZE& rhs) const {
			if (cx < rhs.cx) return true;
			if (cx > rhs.cx) return false;
			return cy < rhs.cy;
		}

		[[nodiscard]] bool operator>=(const SIZE& rhs) const { return !(*this < static_cast<Resolution>(rhs)); }

		[[nodiscard]] bool operator>(const SIZE& rhs) const { return static_cast<Resolution>(rhs) < *this; }

		[[nodiscard]] bool operator<=(const SIZE& rhs) const { return !(*this > static_cast<Resolution>(rhs)); }

		[[nodiscard]] bool operator==(const SIZE& rhs) const { return cx == rhs.cx && cy == rhs.cy; }

		[[nodiscard]] bool operator!=(const SIZE& rhs) const { return !(*this == rhs); }
	};

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

	[[nodiscard]] inline BOOL WINAPI GetDisplayResolutions(set<Resolution>& resolutions, LPCWSTR lpDeviceName = nullptr) {
		set<Resolution> temp;
		Resolution resolution;
		for (DWORD i = 0; GetDisplayResolution(i, resolution, lpDeviceName); i++) temp.emplace(resolution);
		const auto ret = !temp.empty();
		if (ret) resolutions = move(temp);
		return ret;
	}

	[[nodiscard]] inline BOOL WINAPI GetDisplayResolutions(set<Resolution>& resolutions, HMONITOR hMonitor) {
		MONITORINFOEXW monitorInfoEx;
		monitorInfoEx.cbSize = sizeof(monitorInfoEx);
		return GetMonitorInfoW(hMonitor, &monitorInfoEx) && GetDisplayResolutions(resolutions, monitorInfoEx.szDevice);
	}

	[[nodiscard]] inline BOOL WINAPI GetDisplayResolutions(set<Resolution>& resolutions, HWND hWnd) {
		const auto monitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
		return monitor != nullptr && GetDisplayResolutions(resolutions, monitor);
	}
}
