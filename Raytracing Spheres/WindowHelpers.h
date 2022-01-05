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

	inline BOOL WINAPI CenterWindow(HWND hWnd) {
		const auto parent = GetParent(hWnd);
		if (parent != nullptr) {
			RECT border, rect;
			if (GetWindowRect(parent, &border) && GetWindowRect(hWnd, &rect)) {
				CenterRect(border, rect);
				return SetWindowPos(hWnd, nullptr, static_cast<int>(rect.left), static_cast<int>(rect.top), 0, 0, SWP_NOSIZE);
			}
		}
		return FALSE;
	}

	struct WindowModeHelper {
		enum class Mode { Windowed, Borderless, Fullscreen };

		HWND Window{};

		DWORD ExStyle{}, Style = WS_OVERLAPPEDWINDOW;
		BOOL HasMenu{};

		SIZE ClientSize{};

		bool SetMode(Mode mode) {
			const auto ret = m_currentMode != mode;
			if (ret) {
				m_previousMode = m_currentMode;
				m_currentMode = mode;
			}
			return ret;
		}

		Mode GetMode() const { return m_currentMode; }

		void ToggleMode() { SetMode(m_currentMode == Mode::Fullscreen ? m_previousMode : Mode::Fullscreen); }

		BOOL Apply() const {
			const auto ret = (SetWindowLongPtrW(Window, GWL_EXSTYLE, static_cast<LONG_PTR>(m_currentMode == Mode::Fullscreen ? ExStyle | WS_EX_TOPMOST : ExStyle)) || GetLastError() == ERROR_SUCCESS)
				&& (SetWindowLongPtrW(Window, GWL_STYLE, static_cast<LONG_PTR>(m_currentMode == Mode::Windowed ? Style : Style & ~WS_OVERLAPPEDWINDOW)) || GetLastError() == ERROR_SUCCESS)
				&& ResizeWindow();
			if (ret) ShowWindow(Window, m_currentMode == Mode::Fullscreen ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
			return ret;
		}

	private:
		Mode m_previousMode = Mode::Fullscreen, m_currentMode = Mode::Windowed;

		BOOL ResizeWindow() const {
			MONITORINFO monitorInfo;
			monitorInfo.cbSize = sizeof(monitorInfo);
			if (!GetMonitorInfoW(MonitorFromWindow(Window, MONITOR_DEFAULTTONEAREST), &monitorInfo)) return FALSE;
			RECT rc{ 0, 0, ClientSize.cx, ClientSize.cy };
			CenterRect(monitorInfo.rcMonitor, rc);
			return AdjustWindowRectEx(&rc, m_currentMode == Mode::Windowed ? Style : Style & ~WS_OVERLAPPEDWINDOW, HasMenu, ExStyle)
				&& SetWindowPos(Window, HWND_TOP, static_cast<int>(rc.left), static_cast<int>(rc.top), static_cast<int>(rc.right - rc.left), static_cast<int>(rc.bottom - rc.top), SWP_NOZORDER | SWP_FRAMECHANGED);
		}
	};
}
