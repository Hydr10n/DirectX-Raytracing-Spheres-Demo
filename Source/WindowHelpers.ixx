module;

#include <algorithm>

#include <Windows.h>
#include <windowsx.h>

#include <dwmapi.h>

export module WindowHelpers;

import DisplayHelpers;

using namespace DisplayHelpers;
using namespace std;

namespace {
	constexpr Resolution CalculateResolution(const RECT& value) { return { value.right - value.left, value.bottom - value.top }; }
}

export namespace WindowHelpers {
	constexpr void CenterRect(_In_ const RECT& bounds, _Inout_ RECT& rect) {
		const auto size = CalculateResolution(rect);
		rect.left = (bounds.right + bounds.left - size.cx) / 2;
		rect.top = (bounds.bottom + bounds.top - size.cy) / 2;
		rect.right = rect.left + size.cx;
		rect.bottom = rect.top + size.cy;
	}

	enum class WindowMode { Windowed, Borderless, Fullscreen };

	class WindowModeHelper {
	public:
		const HWND hWnd;

		explicit WindowModeHelper(HWND hWnd) : hWnd(hWnd) {}

		bool IsFullscreenResolutionHandledByWindow() const { return m_isFullscreenResolutionHandledByWindow; }

		void SetFullscreenResolutionHandledByWindow(bool value) {
			if (m_isApplying) return;
			m_isFullscreenResolutionHandledByWindow = value;
		}

		void GetWindowedStyles(DWORD& windowedStyle, DWORD& windowedExStyle) const {
			windowedStyle = m_windowedStyle;
			windowedExStyle = m_windowedExStyle;
		}

		void SetWindowedStyles(DWORD windowedStyle, DWORD windowedExStyle) {
			if (m_isApplying) return;
			m_windowedStyle = windowedStyle | WS_CAPTION;
			m_windowedExStyle = windowedExStyle;
		}

		WindowMode GetMode() const { return m_mode; }

		void SetMode(WindowMode value) {
			if (m_isApplying || m_mode == value) return;
			m_previousMode = m_mode;
			m_mode = value;
		}

		void ToggleMode() { SetMode(m_mode == WindowMode::Fullscreen ? m_previousMode : WindowMode::Fullscreen); }

		Resolution GetResolution() const { return m_resolution; }

		void SetResolution(const Resolution& value) {
			if (m_isApplying) return;
			m_resolution = value;
		}

		[[nodiscard]] BOOL Apply() {
			if (m_isApplying) {
				SetLastError(ERROR_INVALID_FUNCTION);
				return FALSE;
			}

			m_isApplying = true;

			WINDOWPLACEMENT windowPlacement;
			windowPlacement.length = sizeof(windowPlacement);
			RECT clientRect;
			if (!GetWindowPlacement(hWnd, &windowPlacement) || !GetClientRect(hWnd, &clientRect)) return FALSE;
			MapWindowRect(hWnd, HWND_DESKTOP, &clientRect);

			DWORD offStyle = 0, offExStyle = 0;
			auto style = m_windowedStyle, exStyle = m_windowedExStyle;
			if (m_mode != WindowMode::Windowed) {
				style &= ~(offStyle = WS_OVERLAPPEDWINDOW);
				exStyle &= ~(offExStyle = WS_EX_OVERLAPPEDWINDOW | WS_EX_STATICEDGE);
			}

			const auto SetWindowStyles = [&] {
				if (const auto currentStyle = GetWindowStyle(hWnd), currentExStyle = GetWindowExStyle(hWnd);
					!(currentStyle & offStyle) && (currentStyle & style) == style &&
					!(currentExStyle & offExStyle) && (currentExStyle & exStyle) == exStyle) {
					return true;
				}

				SetLastError(ERROR_SUCCESS);
				return (SetWindowLongPtrW(hWnd, GWL_EXSTYLE, exStyle) || GetLastError() == ERROR_SUCCESS)
					&& (SetWindowLongPtrW(hWnd, GWL_STYLE, style) || GetLastError() == ERROR_SUCCESS)
					&& (m_mode != WindowMode::Windowed || SetWindowPos(hWnd, nullptr, 0, 0, 0, 0, SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE));
			};

			const auto SetWindowPos = [&] {
				const auto SetWindowPos = [&](const RECT& rect) {
					return ::SetWindowPos(hWnd, nullptr, static_cast<int>(rect.left), static_cast<int>(rect.top), static_cast<int>(rect.right - rect.left), static_cast<int>(rect.bottom - rect.top), SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW) != FALSE;
				};

				RECT displayRect;
				if (!GetDisplayRect(displayRect, hWnd)) return false;

				const auto displayResolution = CalculateResolution(displayRect);

				if (m_mode == WindowMode::Fullscreen) {
					m_previousWindowResolution = CalculateResolution(windowPlacement.rcNormalPosition);

					if (m_isFullscreenResolutionHandledByWindow) m_resolution = displayResolution;

					ShowWindow(hWnd, SW_MAXIMIZE);

					RECT clientRect;
					if (!GetClientRect(hWnd, &clientRect)) return false;

					const auto ret = CalculateResolution(clientRect) == displayResolution || SetWindowPos(displayRect);
					if (ret && !m_isFullscreenResolutionHandledByWindow) {
						SendMessageW(hWnd, WM_SIZE, SIZE_MAXIMIZED, MAKELPARAM(displayResolution.cx, displayResolution.cy));
					}
					return ret;
				}

				m_resolution = min(m_resolution, displayResolution);

				RECT windowRect{ 0, 0, m_resolution.cx, m_resolution.cy };
				AdjustWindowRectExForDpi(&windowRect, style, GetMenu(hWnd) != nullptr, exStyle, GetDpiForWindow(hWnd));

				const auto windowResolution = CalculateResolution(windowRect);

				if (const auto resolution = CalculateResolution(windowPlacement.rcNormalPosition);
					m_previousMode == WindowMode::Fullscreen && CalculateResolution(clientRect) == displayResolution
					&& ((m_isFullscreenResolutionHandledByWindow && resolution == m_previousWindowResolution)
						|| (!m_isFullscreenResolutionHandledByWindow && resolution == windowResolution))) {
					m_isApplying = false;

					windowPlacement.showCmd = SW_NORMAL;
					return SetWindowPlacement(hWnd, &windowPlacement) != FALSE;
				}

				RECT newClientRect;
				if (!GetClientRect(hWnd, &newClientRect)) return false;
				MapWindowRect(hWnd, HWND_DESKTOP, &newClientRect);

				RECT margin;
				if (FAILED(DwmGetWindowAttribute(hWnd, DWMWA_EXTENDED_FRAME_BOUNDS, &margin, sizeof(margin)))) {
					SetLastError(ERROR_CAN_NOT_COMPLETE);
					return false;
				}
				margin = {
					newClientRect.left - margin.left, newClientRect.top - margin.top,
					margin.right - newClientRect.right, margin.bottom - newClientRect.bottom
				};

				windowRect.left += m_resolution.cx + margin.left + margin.right - displayResolution.cx < 0 ?
					clamp((clientRect.right + clientRect.left - m_resolution.cx) / 2, displayRect.left + margin.left, displayRect.right - margin.right - m_resolution.cx) :
					(displayRect.right + displayRect.left - m_resolution.cx) / 2;
				windowRect.top += m_resolution.cy + margin.top + margin.bottom - displayResolution.cy < 0 ?
					clamp((clientRect.bottom + clientRect.top - m_resolution.cy) / 2, displayRect.top + margin.top, displayRect.bottom - margin.bottom - m_resolution.cy) :
					(displayRect.bottom + displayRect.top - m_resolution.cy) / 2;
				windowRect.right = windowRect.left + windowResolution.cx;
				windowRect.bottom = windowRect.top + windowResolution.cy;
				return SetWindowPos(windowRect);
			};

			const auto ret = SetWindowStyles() && SetWindowPos();

			m_isApplying = false;

			return ret;
		}

	private:
		bool m_isApplying{}, m_isFullscreenResolutionHandledByWindow = true;

		DWORD m_windowedStyle = WS_OVERLAPPEDWINDOW, m_windowedExStyle{};

		WindowMode m_previousMode = WindowMode::Windowed, m_mode = WindowMode::Windowed;

		Resolution m_previousWindowResolution{}, m_resolution{};
	};
}
