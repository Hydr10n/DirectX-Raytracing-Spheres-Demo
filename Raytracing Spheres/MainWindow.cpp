#pragma warning(disable: 26812)

#include "MainWindow.h"

#include "MyAppData.h"

#include "resource.h"

using namespace DirectX;
using namespace DX;
using namespace DisplayHelpers;
using namespace std;
using namespace WindowHelpers;

using SettingsData = MyAppData::Settings;
using SettingsKeys = SettingsData::Keys;

namespace MenuID {
	enum {
		WindowModeWindowed, WindowModeBorderless, WindowModeFullscreen,
		AntiAliasingSampleCount1, AntiAliasingSampleCount2, AntiAliasingSampleCount4, AntiAliasingSampleCount8,
		ViewSourceCodeOnGitHub,
		Exit,
		FirstResolution
	};
}

MainWindow::MainWindow() noexcept(false) :
	WindowBase(
		WNDCLASSEXW{
			.hIcon = LoadIcon(GetModuleHandle(nullptr), MAKEINTRESOURCE(IDI_ICON_DIRECTX)),
			.lpszClassName = L"Direct3D 12"
		},
		DefaultTitle,
		0, WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
		nullptr
	) {

	ThrowIfFailed(GetDisplayResolutions(m_displayResolutions));

	const auto hWnd = GetHandle();

	SIZE outputSize;
	if (SettingsData::Load<SettingsKeys::Resolution>(outputSize)) {
		const auto& maxDisplayResolution = *max_element(m_displayResolutions.cbegin(), m_displayResolutions.cend());
		if (outputSize > maxDisplayResolution) outputSize = maxDisplayResolution;
	}
	else {
		RECT rc;
		ThrowIfFailed(GetClientRect(hWnd, &rc));
		outputSize = { rc.right - rc.left, rc.bottom - rc.top };
	}

	m_app = make_unique<decltype(m_app)::element_type>(hWnd, outputSize);

	m_windowModeHelper.Window = hWnd;
	m_windowModeHelper.Style = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
	m_windowModeHelper.ClientSize = outputSize;

	if (!SettingsData::Load<SettingsKeys::AntiAliasingSampleCount>(m_antiAliasingSampleCount) || !m_app->SetAntiAliasingSampleCount(m_antiAliasingSampleCount)) {
		m_app->SetAntiAliasingSampleCount(m_antiAliasingSampleCount = 2);
	}
}

WPARAM MainWindow::Run() {
	m_windowModeHelper.Apply(); // Fix missing icon on title bar when initial WindowMode != Windowed

	WindowMode windowMode;
	if (SettingsData::Load<SettingsData::Keys::WindowMode>(windowMode) && m_windowModeHelper.SetMode(windowMode)) {
		m_windowModeHelper.Apply();
	}

	MSG msg{ .message = WM_QUIT };
	do {
		if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		else m_app->Tick();
	} while (msg.message != WM_QUIT);
	return msg.wParam;
}

LRESULT MainWindow::OnMessageReceived(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
	switch (uMsg) {
	case WM_CONTEXTMENU: {
		CURSORINFO cursorInfo{ sizeof(cursorInfo) };
		const auto isCursorVisible = GetCursorInfo(&cursorInfo) ? (cursorInfo.flags & CURSOR_SHOWING) != 0 : true;
		RECT clipCursorRect;
		GetClipCursor(&clipCursorRect);
		while (ShowCursor(TRUE) < 0) {}
		ClipCursor(nullptr);

		const auto menu = CreatePopupMenu(), hMenuWindowMode = CreatePopupMenu(), hMenuResolution = CreatePopupMenu(), hMenuAntiAliasing = CreatePopupMenu();

		const auto mode = m_windowModeHelper.GetMode();
		AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(hMenuWindowMode), L"Window Mode");
		AppendMenuW(hMenuWindowMode, MF_STRING | (mode == WindowMode::Windowed ? MF_CHECKED : MF_UNCHECKED), MenuID::WindowModeWindowed, L"Windowed");
		AppendMenuW(hMenuWindowMode, MF_STRING | (mode == WindowMode::Borderless ? MF_CHECKED : MF_UNCHECKED), MenuID::WindowModeBorderless, L"Borderless");
		AppendMenuW(hMenuWindowMode, MF_STRING | (mode == WindowMode::Fullscreen ? MF_CHECKED : MF_UNCHECKED), MenuID::WindowModeFullscreen, L"Fullscreen");

		AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(hMenuResolution), L"Resolution");
		for (size_t size = m_displayResolutions.size(), i = 0; i < size; i++) {
			AppendMenuW(hMenuResolution, MF_STRING | (m_windowModeHelper.ClientSize == m_displayResolutions[i] ? MF_CHECKED : MF_UNCHECKED), MenuID::FirstResolution + i, (to_wstring(m_displayResolutions[i].cx) + L" × " + to_wstring(m_displayResolutions[i].cy)).c_str());
		}

		AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

		AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(hMenuAntiAliasing), L"Anti-Aliasing");
		AppendMenuW(hMenuAntiAliasing, MF_STRING | (m_antiAliasingSampleCount == 1 ? MF_CHECKED : MF_UNCHECKED), MenuID::AntiAliasingSampleCount1, L"Off");
		AppendMenuW(hMenuAntiAliasing, MF_STRING | (m_antiAliasingSampleCount == 2 ? MF_CHECKED : MF_UNCHECKED), MenuID::AntiAliasingSampleCount2, L"2x");
		AppendMenuW(hMenuAntiAliasing, MF_STRING | (m_antiAliasingSampleCount == 4 ? MF_CHECKED : MF_UNCHECKED), MenuID::AntiAliasingSampleCount4, L"4x");
		AppendMenuW(hMenuAntiAliasing, MF_STRING | (m_antiAliasingSampleCount == 8 ? MF_CHECKED : MF_UNCHECKED), MenuID::AntiAliasingSampleCount8, L"8x");

		AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

		AppendMenuW(menu, MF_STRING, MenuID::ViewSourceCodeOnGitHub, L"View Source Code on GitHub");

		AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

		AppendMenuW(menu, MF_POPUP, MenuID::Exit, L"Exit");

		RECT rc{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		if (rc.left == -1 && rc.top == -1) {
			GetClientRect(hWnd, &rc);
			MapWindowRect(hWnd, HWND_DESKTOP, &rc);
		}
		TrackPopupMenu(menu, TPM_LEFTALIGN, static_cast<int>(rc.left), static_cast<int>(rc.top), 0, hWnd, nullptr);

		DestroyMenu(menu);

		if (GetForegroundWindow() == hWnd) ClipCursor(&clipCursorRect);
		ShowCursor(isCursorVisible);
	}	break;

	case WM_COMMAND: {
		const auto SetWindowMode = [&](WindowMode mode) {
			if (m_windowModeHelper.SetMode(mode) && m_windowModeHelper.Apply()) SettingsData::Save<SettingsKeys::WindowMode>(mode);
		};

		switch (const auto menuID = LOWORD(wParam)) {
		case MenuID::WindowModeWindowed: SetWindowMode(WindowMode::Windowed); break;
		case MenuID::WindowModeBorderless: SetWindowMode(WindowMode::Borderless); break;
		case MenuID::WindowModeFullscreen: SetWindowMode(WindowMode::Fullscreen); break;

		case MenuID::AntiAliasingSampleCount1:
		case MenuID::AntiAliasingSampleCount2:
		case MenuID::AntiAliasingSampleCount4:
		case MenuID::AntiAliasingSampleCount8: {
			const auto antiAliasingSampleCount = menuID == MenuID::AntiAliasingSampleCount1 ? 1 :
				menuID == MenuID::AntiAliasingSampleCount2 ? 2 :
				menuID == MenuID::AntiAliasingSampleCount4 ? 4 :
				8;
			if (m_app->SetAntiAliasingSampleCount(antiAliasingSampleCount)) {
				m_antiAliasingSampleCount = antiAliasingSampleCount;

				SettingsData::Save<SettingsData::Keys::AntiAliasingSampleCount>(antiAliasingSampleCount);
			}
		}	break;

		case MenuID::ViewSourceCodeOnGitHub: ShellExecuteW(nullptr, L"open", L"https://github.com/Hydr10n/DirectX-Raytracing-Spheres-Demo", nullptr, nullptr, SW_SHOW); break;

		case MenuID::Exit: SendMessageW(hWnd, WM_CLOSE, 0, 0); break;

		default: {
			const auto& resolution = m_displayResolutions[menuID - MenuID::FirstResolution];
			if (resolution != m_windowModeHelper.ClientSize) {
				m_windowModeHelper.ClientSize = resolution;
				if (m_windowModeHelper.Apply()) {
					m_app->OnWindowSizeChanged(resolution);

					SettingsData::Save<SettingsKeys::Resolution>(resolution);
				}
			}
		}	break;
		}
	}	break;

	case WM_GETMINMAXINFO: if (lParam) reinterpret_cast<PMINMAXINFO>(lParam)->ptMinTrackSize = { 320, 200 }; break;

	case WM_MOVING:
	case WM_SIZING: m_app->Tick(); break;

	case WM_SIZE: {
		switch (wParam) {
		case SIZE_MINIMIZED: m_app->OnSuspending(); break;
		case SIZE_RESTORED: m_app->OnResuming(); [[fallthrough]];
		default: {
			m_app->OnWindowSizeChanged(m_windowModeHelper.ClientSize);

			RECT rc{ 0, 0, LOWORD(lParam), HIWORD(lParam) };
			MapWindowRect(hWnd, HWND_DESKTOP, &rc);
			ClipCursor(&rc);
		} break;
		}
	}	break;

	case WM_DPICHANGED: {
		const auto rc = reinterpret_cast<PRECT>(lParam);
		SetWindowPos(hWnd, nullptr, static_cast<int>(rc->left), static_cast<int>(rc->top), static_cast<int>(rc->right - rc->left), static_cast<int>(rc->bottom - rc->top), SWP_NOZORDER);
	}	break;

	case WM_SYSKEYDOWN: {
		if (wParam == VK_RETURN && (lParam & 0x60000000) == 0x20000000) {
			m_windowModeHelper.ToggleMode();
			if (m_windowModeHelper.Apply()) SettingsData::Save<SettingsKeys::WindowMode>(m_windowModeHelper.GetMode());
		}
	} [[fallthrough]];
	case WM_SYSKEYUP:
	case WM_KEYDOWN:
	case WM_KEYUP: Keyboard::ProcessMessage(uMsg, wParam, lParam); break;

	case WM_LBUTTONDOWN:
		//case WM_RBUTTONDOWN:
	case WM_MBUTTONDOWN:
	case WM_XBUTTONDOWN: {
		SetCapture(hWnd);

		Mouse::ProcessMessage(uMsg, wParam, lParam);
	}	break;

	case WM_LBUTTONUP:
		//case WM_RBUTTONUP:
	case WM_MBUTTONUP:
	case WM_XBUTTONUP: ReleaseCapture(); [[fallthrough]];
	case WM_INPUT:
	case WM_MOUSEMOVE:
	case WM_MOUSEWHEEL:
	case WM_MOUSEHOVER: Mouse::ProcessMessage(uMsg, wParam, lParam); break;

	case WM_ACTIVATEAPP: {
		if (wParam) {
			m_app->OnActivated();

			SetForegroundWindow(hWnd);
		}
		else m_app->OnDeactivated();
	} [[fallthrough]];
	case WM_ACTIVATE: {
		Keyboard::ProcessMessage(uMsg, wParam, lParam);
		Mouse::ProcessMessage(uMsg, wParam, lParam);
	}	break;

	case WM_MENUCHAR: return MAKELRESULT(0, MNC_CLOSE);

		//case WM_MOUSEACTIVATE: return MA_ACTIVATEANDEAT;

	case WM_DESTROY: PostQuitMessage(ERROR_SUCCESS); break;

	default: return DefWindowProcW(hWnd, uMsg, wParam, lParam);
	}

	return 0;
}