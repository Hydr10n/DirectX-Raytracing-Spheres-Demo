#pragma once

#include "DisplayHelpers.h"

namespace WindowHelpers {
	constexpr void CenterRect(_In_ const RECT& border, _Inout_ RECT& rect) {
		const auto rectWidth = rect.right - rect.left, rectHeight = rect.bottom - rect.top;
		rect.left = (border.right + border.left - rectWidth) / 2;
		rect.top = (border.bottom + border.top - rectHeight) / 2;
		rect.right = rect.left + rectWidth;
		rect.bottom = rect.top + rectHeight;
	}

	enum class WindowMode { Windowed, Borderless, Fullscreen };

	class WindowModeHelper {
	public:
		const HWND hWnd;

		DWORD WindowedStyle = WS_OVERLAPPEDWINDOW, WindowedExStyle{};

		DisplayHelpers::Resolution Resolution{};

		WindowModeHelper(HWND hWnd) : hWnd(hWnd) {}

		void SetMode(WindowMode mode) {
			if (m_currentMode == mode) return;
			m_previousMode = m_currentMode;
			m_currentMode = mode;
		}

		void ToggleMode() { SetMode(m_currentMode == WindowMode::Fullscreen ? m_previousMode : WindowMode::Fullscreen); }

		WindowMode GetMode() const { return m_currentMode; }

		BOOL Apply() const {
			const auto
				style = m_currentMode == WindowMode::Windowed ? WindowedStyle | WS_CAPTION : WindowedStyle & ~WS_OVERLAPPEDWINDOW,
				exStyle = m_currentMode == WindowMode::Fullscreen ? WindowedExStyle | WS_EX_TOPMOST : WindowedExStyle;

			const auto SetWindowStyles = [&] {
				SetLastError(ERROR_SUCCESS);
				return (SetWindowLongPtrW(hWnd, GWL_EXSTYLE, static_cast<LONG_PTR>(exStyle)) || GetLastError() == ERROR_SUCCESS)
					&& (SetWindowLongPtrW(hWnd, GWL_STYLE, static_cast<LONG_PTR>(style)) || GetLastError() == ERROR_SUCCESS);
			};

			const auto SetWindowSize = [&] {
				RECT displayRect;
				if (!DisplayHelpers::GetDisplayRect(displayRect, hWnd)) return false;
				RECT rect{ 0, 0, Resolution.cx, Resolution.cy };
				CenterRect(displayRect, rect);
				return AdjustWindowRectExForDpi(&rect, style, GetMenu(hWnd) != nullptr, exStyle, GetDpiForWindow(hWnd))
					&& SetWindowPos(hWnd, HWND_TOP, static_cast<int>(rect.left), static_cast<int>(rect.top), static_cast<int>(rect.right - rect.left), static_cast<int>(rect.bottom - rect.top), SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW) != 0;
			};

			auto ret = SetWindowStyles();
			if (ret) {
				if (m_currentMode == WindowMode::Fullscreen) ShowWindow(hWnd, SW_SHOWMAXIMIZED);
				else ret = SetWindowSize();
			}
			return ret;
		}

	private:
		WindowMode m_previousMode = WindowMode::Fullscreen, m_currentMode = WindowMode::Windowed;
	};
}
