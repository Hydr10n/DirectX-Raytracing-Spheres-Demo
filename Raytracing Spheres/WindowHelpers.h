#pragma once

#include <Windows.h>

namespace WindowHelpers {
	constexpr void CenterRect(_In_ const RECT& border, _Inout_ RECT& rect) {
		const auto rectWidth = rect.right - rect.left, rectHeight = rect.bottom - rect.top;
		rect.left = (border.right + border.left - rectWidth) / 2;
		rect.top = (border.bottom + border.top - rectHeight) / 2;
		rect.right = rect.left + rectWidth;
		rect.bottom = rect.top + rectHeight;
	}

	enum class WindowMode { Windowed, Borderless, Fullscreen };

	struct WindowModeHelper {
		HWND Window{};

		DWORD ExStyle{}, Style = WS_OVERLAPPEDWINDOW;
		BOOL HasMenu{};

		SIZE ClientSize{};

		bool SetMode(WindowMode mode) {
			const auto ret = m_currentMode != mode;
			if (ret) {
				m_previousMode = m_currentMode;
				m_currentMode = mode;
			}
			return ret;
		}

		WindowMode GetMode() const { return m_currentMode; }

		void ToggleMode() { SetMode(m_currentMode == WindowMode::Fullscreen ? m_previousMode : WindowMode::Fullscreen); }

		BOOL Apply() const {
			const auto exStyle = m_currentMode == WindowMode::Fullscreen ? ExStyle | WS_EX_TOPMOST : ExStyle,
				style = m_currentMode == WindowMode::Windowed ? Style | WS_CAPTION : Style & ~WS_OVERLAPPEDWINDOW;
			const auto SetWindowStyles = [&]() -> BOOL {
				return (SetWindowLongPtrW(Window, GWL_EXSTYLE, static_cast<LONG_PTR>(exStyle)) || GetLastError() == ERROR_SUCCESS)
					&& (SetWindowLongPtrW(Window, GWL_STYLE, static_cast<LONG_PTR>(style)) || GetLastError() == ERROR_SUCCESS);
			};
			const auto ResizeWindow = [&]() -> BOOL {
				MONITORINFO monitorInfo;
				monitorInfo.cbSize = sizeof(monitorInfo);
				if (!GetMonitorInfoW(MonitorFromWindow(Window, MONITOR_DEFAULTTONEAREST), &monitorInfo)) return FALSE;
				RECT rc{ 0, 0, ClientSize.cx, ClientSize.cy };
				CenterRect(monitorInfo.rcMonitor, rc);
				return AdjustWindowRectEx(&rc, style, HasMenu, exStyle)
					&& SetWindowPos(Window, HWND_TOP, static_cast<int>(rc.left), static_cast<int>(rc.top), static_cast<int>(rc.right - rc.left), static_cast<int>(rc.bottom - rc.top), SWP_NOZORDER | SWP_FRAMECHANGED);
			};
			const auto ret = SetWindowStyles() && ResizeWindow();
			if (ret) ShowWindow(Window, m_currentMode == WindowMode::Fullscreen ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
			return ret;
		}

	private:
		WindowMode m_previousMode = WindowMode::Fullscreen, m_currentMode = WindowMode::Windowed;
	};
}
