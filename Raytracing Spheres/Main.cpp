#include "pch.h"

#include "directxtk12/Keyboard.h"
#include "directxtk12/Mouse.h"

#include "imgui_impl_win32.h"

#include "MyAppData.h"

#include "resource.h"

#include <set>

import D3DApp;
import DisplayHelpers;
import SharedData;
import WindowHelpers;

using namespace DirectX;
using namespace DisplayHelpers;
using namespace DX;
using namespace Microsoft::WRL::Wrappers;
using namespace std;
using namespace WindowHelpers;

using GraphicsSettingsData = MyAppData::Settings::Graphics;

constexpr auto DefaultWindowTitle = L"Raytracing Spheres";

exception_ptr g_exception;

shared_ptr<WindowModeHelper> g_windowModeHelper;

unique_ptr<D3DApp> g_app;

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

int WINAPI wWinMain(
	[[maybe_unused]] _In_ HINSTANCE hInstance, _In_opt_ HINSTANCE,
	[[maybe_unused]] _In_ LPWSTR lpCmdLine, [[maybe_unused]] _In_ int nShowCmd
) {
	if (!XMVerifyCPUSupport()) {
		MessageBoxW(nullptr, L"DirectXMath is not supported by CPU.", nullptr, MB_OK | MB_ICONERROR);
		return ERROR_CAN_NOT_COMPLETE;
	}

	int ret;

	RoInitializeWrapper roInitializeWrapper(RO_INIT_MULTITHREADED);

	try {
		ThrowIfFailed(static_cast<HRESULT>(roInitializeWrapper));

		const WNDCLASSEXW wndClassEx{
			.cbSize = sizeof(wndClassEx),
			.lpfnWndProc = WndProc,
			.hInstance = hInstance,
			.hIcon = LoadIcon(GetModuleHandle(nullptr), MAKEINTRESOURCE(IDI_ICON_DIRECTX)),
			.hCursor = LoadCursor(nullptr, IDC_ARROW),
			.lpszClassName = L"Direct3D 12"
		};
		ThrowIfFailed(static_cast<BOOL>(RegisterClassExW(&wndClassEx)));

		const auto window = CreateWindowExW(
			0,
			wndClassEx.lpszClassName,
			DefaultWindowTitle,
			WS_OVERLAPPEDWINDOW,
			CW_USEDEFAULT, CW_USEDEFAULT,
			CW_USEDEFAULT, CW_USEDEFAULT,
			nullptr,
			nullptr,
			wndClassEx.hInstance,
			nullptr
		);
		ThrowIfFailed(static_cast<BOOL>(window != nullptr));

		g_windowModeHelper = make_shared<decltype(g_windowModeHelper)::element_type>(window);

		g_windowModeHelper->WindowedStyle = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;

		if (!GraphicsSettingsData::Load<GraphicsSettingsData::Resolution>(g_windowModeHelper->Resolution)
			|| !g_displayResolutions.contains(g_windowModeHelper->Resolution)) {
			RECT rect;
			ThrowIfFailed(GetClientRect(g_windowModeHelper->hWnd, &rect));
			g_windowModeHelper->Resolution = { rect.right - rect.left, rect.bottom - rect.top };
		}

		g_app = make_unique<decltype(g_app)::element_type>(g_windowModeHelper);

		ThrowIfFailed(g_windowModeHelper->Apply()); // Fix missing icon on title bar when initial WindowMode != Windowed

		if (WindowMode windowMode; GraphicsSettingsData::Load<GraphicsSettingsData::WindowMode>(windowMode)) {
			g_windowModeHelper->SetMode(windowMode);
			ThrowIfFailed(g_windowModeHelper->Apply());
		}

		MSG msg{ .message = WM_QUIT };
		do {
			if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
				TranslateMessage(&msg);
				DispatchMessageW(&msg);

				if (g_exception) rethrow_exception(g_exception);
			}
			else g_app->Tick();
		} while (msg.message != WM_QUIT);
		ret = static_cast<decltype(ret)>(msg.wParam);
	}
	catch (const system_error& e) {
		ret = e.code().value();
		MessageBoxA(nullptr, e.what(), nullptr, MB_OK | MB_ICONERROR);
	}
	catch (const exception& e) {
		ret = ERROR_CAN_NOT_COMPLETE;
		MessageBoxA(nullptr, e.what(), nullptr, MB_OK | MB_ICONERROR);
	}
	catch (...) {
		ret = static_cast<int>(GetLastError());
		if (ret != ERROR_SUCCESS) MessageBoxA(nullptr, system_category().message(ret).c_str(), nullptr, MB_OK | MB_ICONERROR);
	}

	g_app.reset();

	return ret;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
	try {
		{
			LPARAM param;
			if (uMsg == WM_MOUSEMOVE) {
				const auto [cx, cy] = g_app->GetOutputSize();
				RECT rect;
				ThrowIfFailed(GetClientRect(hWnd, &rect));
				param = MAKELPARAM(GET_X_LPARAM(lParam) * cx / static_cast<float>(rect.right - rect.left), GET_Y_LPARAM(lParam) * cy / static_cast<float>(rect.bottom - rect.top));
			}
			else param = lParam;

			extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
			if (const auto ret = ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, param)) return ret;
		}

		static HMONITOR s_hMonitor;

		static Resolution s_displayResolution;

		const auto GetDisplayResolutions = [&](bool forceUpdate = false) {
			const auto monitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
			ThrowIfFailed(static_cast<BOOL>(monitor != nullptr));

			const auto ret = monitor != s_hMonitor;
			if (ret || forceUpdate) {
				ThrowIfFailed(::GetDisplayResolutions(g_displayResolutions, monitor));

				if (const auto resolution = g_displayResolutions.cbegin()->IsPortrait() ? Resolution{ 600, 800 } : Resolution{ 800, 600 };
					*--g_displayResolutions.cend() > resolution) {
					erase_if(g_displayResolutions, [&](const auto& displayResolution) { return displayResolution < resolution; });
				}

				ThrowIfFailed(GetDisplayResolution(s_displayResolution, monitor));

				s_hMonitor = monitor;
			}
			return ret;
		};

		if (s_hMonitor == nullptr) GetDisplayResolutions();

		switch (uMsg) {
		case WM_GETMINMAXINFO: {
			if (lParam) {
				const auto style = GetWindowStyle(hWnd), exStyle = GetWindowExStyle(hWnd);
				const auto hasMenu = GetMenu(hWnd) != nullptr;
				const auto DPI = GetDpiForWindow(hWnd);

				const auto AdjustSize = [&](const auto& size, auto& newSize) {
					RECT rect{ 0, 0, size.cx, size.cy };
					if (AdjustWindowRectExForDpi(&rect, style, hasMenu, exStyle, DPI)) {
						newSize = { rect.right - rect.left, rect.bottom - rect.top };
					}
				};

				auto& minMaxInfo = *reinterpret_cast<PMINMAXINFO>(lParam);
				AdjustSize(*g_displayResolutions.cbegin(), minMaxInfo.ptMinTrackSize);
				AdjustSize(*--g_displayResolutions.cend(), minMaxInfo.ptMaxTrackSize);
			}
		} break;

		case WM_MOVE: GetDisplayResolutions(); break;

		case WM_MOVING:
		case WM_SIZING: g_app->Tick(); break;

		case WM_SIZE: {
			switch (wParam) {
			case SIZE_MINIMIZED: g_app->OnSuspending(); break;

			case SIZE_RESTORED: g_app->OnResuming(); [[fallthrough]];
			default: {
				//g_windowModeHelper->Resolution = { LOWORD(lParam), HIWORD(lParam) };

				g_app->OnWindowSizeChanged();
			} break;
			}
		} break;

		case WM_DISPLAYCHANGE: {
			g_app->OnDisplayChanged();

			if (GetDisplayResolutions()) {
				g_windowModeHelper->Resolution = min(g_windowModeHelper->Resolution, *--g_displayResolutions.cend());

				ThrowIfFailed(g_windowModeHelper->Apply());
			}
			else {
				Resolution displayResolution;
				ThrowIfFailed(GetDisplayResolution(0, displayResolution, s_hMonitor));
				if (displayResolution.IsPortrait() != g_windowModeHelper->Resolution.IsPortrait()) {
					GetDisplayResolutions(true);

					swap(g_windowModeHelper->Resolution.cx, g_windowModeHelper->Resolution.cy);

					ThrowIfFailed(g_windowModeHelper->Apply());
				}
				else if ((displayResolution = { LOWORD(lParam), HIWORD(lParam) }) != s_displayResolution) {
					ThrowIfFailed(g_windowModeHelper->Apply());

					s_displayResolution = displayResolution;
				}
			}
		} break;

		case WM_DPICHANGED: {
			const auto& [left, top, right, bottom] = *reinterpret_cast<PRECT>(lParam);
			SetWindowPos(hWnd, nullptr, static_cast<int>(left), static_cast<int>(top), static_cast<int>(right - left), static_cast<int>(bottom - top), SWP_NOZORDER);
		} break;

		case WM_ACTIVATEAPP: {
			if (wParam) g_app->OnActivated();
			else g_app->OnDeactivated();
		} [[fallthrough]];
		case WM_ACTIVATE: {
			Keyboard::ProcessMessage(uMsg, wParam, lParam);
			Mouse::ProcessMessage(uMsg, wParam, lParam);
		} break;

		case WM_SYSKEYDOWN: {
			if (wParam == VK_RETURN && (lParam & 0x60000000) == 0x20000000) {
				g_windowModeHelper->ToggleMode();
				ThrowIfFailed(g_windowModeHelper->Apply());

				GraphicsSettingsData::Save<GraphicsSettingsData::WindowMode>(g_windowModeHelper->GetMode());
			}
		} [[fallthrough]];
		case WM_SYSKEYUP:
		case WM_KEYDOWN:
		case WM_KEYUP: Keyboard::ProcessMessage(uMsg, wParam, lParam); break;

		case WM_LBUTTONDOWN:
		case WM_RBUTTONDOWN:
		case WM_MBUTTONDOWN:
		case WM_XBUTTONDOWN: {
			SetCapture(hWnd);

			Mouse::ProcessMessage(uMsg, wParam, lParam);
		} break;

		case WM_LBUTTONUP:
		case WM_RBUTTONUP:
		case WM_MBUTTONUP:
		case WM_XBUTTONUP: ReleaseCapture(); [[fallthrough]];
		case WM_INPUT:
		case WM_MOUSEMOVE:
		case WM_MOUSEWHEEL:
		case WM_MOUSEHOVER: Mouse::ProcessMessage(uMsg, wParam, lParam); break;

			//case WM_MOUSEACTIVATE: return MA_ACTIVATEANDEAT;

		case WM_MENUCHAR: return MAKELRESULT(0, MNC_CLOSE);

		case WM_DESTROY: PostQuitMessage(ERROR_SUCCESS); break;

		default: return DefWindowProcW(hWnd, uMsg, wParam, lParam);
		}
	}
	catch (...) { g_exception = current_exception(); }

	return 0;
}
