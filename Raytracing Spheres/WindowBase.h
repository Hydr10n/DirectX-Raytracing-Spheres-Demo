#pragma once

#include "ErrorHelpers.h"

#include <windowsx.h>

namespace Windows {
	class WindowBase {
	public:
		WindowBase(const WindowBase&) = delete;
		WindowBase& operator=(const WindowBase&) = delete;

		HWND GetHandle() const { return m_hWnd; }

		virtual ~WindowBase() = 0 { UnregisterClassW(m_className.c_str(), m_hInstance); }

	private:
		std::wstring m_className;
		HINSTANCE m_hInstance{};

		HWND m_hWnd{};

	protected:
		WindowBase(
			WNDCLASSEXW wndClassEx,
			LPCWSTR lpWindowName,
			DWORD dwExStyle, DWORD dwStyle,
			int X, int Y, int nWidth, int nHeight,
			HWND hWndParent, HMENU hMenu = nullptr
		) noexcept(false) {
			constexpr auto WndProc = [](HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
				const auto _this = reinterpret_cast<decltype(this)>(uMsg == WM_NCCREATE ? reinterpret_cast<LPCREATESTRUCT>(lParam)->lpCreateParams : reinterpret_cast<LPVOID>(GetWindowLongPtrW(hWnd, GWLP_USERDATA)));
				if (_this != nullptr) return _this->OnMessageReceived(hWnd, uMsg, wParam, lParam);
				return DefWindowProcW(hWnd, uMsg, wParam, lParam);
			};

			if (wndClassEx.hInstance == nullptr) wndClassEx.hInstance = GetModuleHandle(nullptr);

			if (!GetClassInfoExW(wndClassEx.hInstance, wndClassEx.lpszClassName, &wndClassEx)) {
				wndClassEx.cbSize = sizeof(wndClassEx);

				wndClassEx.lpfnWndProc = WndProc;

				if (wndClassEx.hCursor == nullptr) wndClassEx.hCursor = LoadCursor(nullptr, IDC_ARROW);

				ErrorHelpers::ThrowIfFailed(RegisterClassExW(&wndClassEx));
			}
			else if (wndClassEx.lpfnWndProc != WndProc) {
				ErrorHelpers::throw_std_system_error(ERROR_CLASS_ALREADY_EXISTS);
			}

			if ((m_hWnd = CreateWindowExW(dwExStyle, wndClassEx.lpszClassName, lpWindowName, dwStyle, X, Y, nWidth, nHeight, hWndParent, hMenu, wndClassEx.hInstance, this)) == nullptr) {
				const auto lastError = GetLastError();

				UnregisterClassW(wndClassEx.lpszClassName, wndClassEx.hInstance);

				SetLastError(lastError);

				ErrorHelpers::throw_std_system_error(lastError);
			}
			else {
				SetWindowLongPtrW(m_hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

				m_className = wndClassEx.lpszClassName;
				m_hInstance = wndClassEx.hInstance;
			}
		}

		virtual LRESULT CALLBACK OnMessageReceived(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) { return DefWindowProcW(hWnd, uMsg, wParam, lParam); }
	};
}
