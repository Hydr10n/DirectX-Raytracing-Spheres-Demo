module;

#include <set>

#include <Windows.h>

export module DisplayHelpers;

using namespace std;

export namespace DisplayHelpers {
	struct Resolution : SIZE {
		bool IsPortrait() const { return cx < cy; }

		[[nodiscard]] auto operator<=>(const Resolution& rhs) const {
			if (cx < rhs.cx) return -1;
			if (cx > rhs.cx) return 1;
			if (cy < rhs.cy) return -1;
			if (cy > rhs.cy) return 1;
			return 0;
		}

		[[nodiscard]] bool operator==(const Resolution& rhs) const { return cx == rhs.cx && cy == rhs.cy; }
	};

	[[nodiscard]] inline BOOL GetDisplayRect(RECT& rect, HMONITOR hMonitor) {
		MONITORINFO monitorInfo;
		monitorInfo.cbSize = sizeof(monitorInfo);
		const auto ret = GetMonitorInfoW(hMonitor, &monitorInfo);
		if (ret) rect = monitorInfo.rcMonitor;
		return ret;
	}

	[[nodiscard]] inline BOOL GetDisplayRect(RECT& rect, HWND hWnd) {
		const auto monitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
		return monitor != nullptr && GetDisplayRect(rect, monitor);
	}

	[[nodiscard]] inline BOOL GetDisplayResolution(Resolution& resolution, HMONITOR hMonitor) {
		RECT rect;
		const auto ret = GetDisplayRect(rect, hMonitor);
		if (ret) resolution = { rect.right - rect.left, rect.bottom - rect.top };
		return ret;
	}

	[[nodiscard]] inline BOOL GetDisplayResolution(Resolution& resolution, HWND hWnd) {
		const auto monitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
		return monitor != nullptr && GetDisplayResolution(resolution, monitor);
	}

	[[nodiscard]] inline BOOL GetDisplayResolution(DWORD index, Resolution& resolution, LPCWSTR lpDeviceName = nullptr) {
		DEVMODEW devMode;
		devMode.dmSize = sizeof(devMode);
		const auto ret = EnumDisplaySettingsW(lpDeviceName, index, &devMode);
		if (ret) resolution = { static_cast<LONG>(devMode.dmPelsWidth), static_cast<LONG>(devMode.dmPelsHeight) };
		return ret;
	}

	[[nodiscard]] inline BOOL GetDisplayResolution(DWORD index, Resolution& resolution, HMONITOR hMonitor) {
		MONITORINFOEXW monitorInfoEx;
		monitorInfoEx.cbSize = sizeof(monitorInfoEx);
		return GetMonitorInfoW(hMonitor, &monitorInfoEx) && GetDisplayResolution(index, resolution, monitorInfoEx.szDevice);
	}

	[[nodiscard]] inline BOOL GetDisplayResolution(DWORD index, Resolution& resolution, HWND hWnd) {
		const auto monitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
		return monitor != nullptr && GetDisplayResolution(index, resolution, monitor);
	}

	[[nodiscard]] inline BOOL GetDisplayResolutions(set<Resolution>& resolutions, LPCWSTR lpDeviceName = nullptr) {
		set<Resolution> temp;
		Resolution resolution;
		for (DWORD i = 0; GetDisplayResolution(i, resolution, lpDeviceName); i++) temp.emplace(resolution);
		const auto ret = !empty(temp);
		if (ret) resolutions = move(temp);
		return ret;
	}

	[[nodiscard]] inline BOOL GetDisplayResolutions(set<Resolution>& resolutions, HMONITOR hMonitor) {
		MONITORINFOEXW monitorInfoEx;
		monitorInfoEx.cbSize = sizeof(monitorInfoEx);
		return GetMonitorInfoW(hMonitor, &monitorInfoEx) && GetDisplayResolutions(resolutions, monitorInfoEx.szDevice);
	}

	[[nodiscard]] inline BOOL GetDisplayResolutions(set<Resolution>& resolutions, HWND hWnd) {
		const auto monitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
		return monitor != nullptr && GetDisplayResolutions(resolutions, monitor);
	}
}
