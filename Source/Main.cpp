#include <set>

#include <Windows.h>
#include <windowsx.h>

#include <wrl.h>

#include "directxtk12/Keyboard.h"
#include "directxtk12/Mouse.h"

#include "nvapi.h"

#include "imgui_impl_win32.h"

#include "MyAppData.h"

#include "resource.h"

import App;
import ErrorHelpers;
import SharedData;

using namespace DirectX;
using namespace DisplayHelpers;
using namespace ErrorHelpers;
using namespace Microsoft::WRL::Wrappers;
using namespace SharedData;
using namespace std;
using namespace WindowHelpers;

namespace {
	constexpr auto& g_graphicsSettings = MyAppData::Settings::Graphics;
}

namespace {
	shared_ptr<WindowModeHelper> g_windowModeHelper;

	unique_ptr<App> g_app;

	exception_ptr g_exception;
}

int WINAPI wWinMain(
	[[maybe_unused]] _In_ HINSTANCE hInstance, _In_opt_ HINSTANCE,
	[[maybe_unused]] _In_ LPWSTR lpCmdLine, [[maybe_unused]] _In_ int nShowCmd
) {
	if (!XMVerifyCPUSupport()) {
		MessageBoxW(nullptr, L"DirectXMath is not supported by CPU.", nullptr, MB_OK | MB_ICONERROR);
		return ERROR_CAN_NOT_COMPLETE;
	}

	int ret;
	string error;

	RoInitializeWrapper roInitializeWrapper(RO_INIT_MULTITHREADED);

	struct NVAPI {
		NVAPI() { ignore = NvAPI_Initialize(); }
		~NVAPI() { ignore = NvAPI_Unload(); }
	} NVAPI;

	try {
		ThrowIfFailed(static_cast<HRESULT>(roInitializeWrapper));

		LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
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
			L"Raytracing Spheres",
			WS_OVERLAPPEDWINDOW,
			CW_USEDEFAULT, CW_USEDEFAULT,
			CW_USEDEFAULT, CW_USEDEFAULT,
			nullptr,
			nullptr,
			wndClassEx.hInstance,
			nullptr
		);
		ThrowIfFailed(static_cast<BOOL>(window != nullptr));

		RECT clientRect;
		if (g_graphicsSettings.Resolution >= *cbegin(g_displayResolutions) && g_graphicsSettings.Resolution <= *--cend(g_displayResolutions)) {
			clientRect = { 0, 0, g_graphicsSettings.Resolution.cx, g_graphicsSettings.Resolution.cy };
		}
		else {
			ThrowIfFailed(GetClientRect(window, &clientRect));

			g_graphicsSettings.Resolution = { clientRect.right - clientRect.left, clientRect.bottom - clientRect.top };
		}

		g_windowModeHelper = make_shared<WindowModeHelper>(window);

		g_windowModeHelper->SetResolution(g_graphicsSettings.Resolution);

		RECT displayRect;
		ThrowIfFailed(GetDisplayRect(displayRect, window));
		CenterRect(displayRect, clientRect);
		AdjustWindowRectExForDpi(&clientRect, GetWindowStyle(window), GetMenu(window) != nullptr, GetWindowExStyle(window), GetDpiForWindow(window));
		SetWindowPos(window, nullptr, static_cast<int>(clientRect.left), static_cast<int>(clientRect.top), static_cast<int>(clientRect.right - clientRect.left), static_cast<int>(clientRect.bottom - clientRect.top), SWP_NOZORDER);

		g_app = make_unique<App>(*g_windowModeHelper);

		// HACK: Fix missing icon on title bar when initial WindowMode != Windowed
		ThrowIfFailed(g_windowModeHelper->Apply());
		if (g_graphicsSettings.WindowMode != WindowMode::Windowed) {
			g_windowModeHelper->SetMode(g_graphicsSettings.WindowMode);
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
		ret = static_cast<int>(msg.wParam);
	}
	catch (const system_error& e) {
		ret = e.code().value();
		error = e.what();
	}
	catch (const exception& e) {
		ret = ERROR_CAN_NOT_COMPLETE;
		error = e.what();
	}
	catch (...) {
		ret = ERROR_CAN_NOT_COMPLETE;
		error = "Unknown exception";
	}

	g_app.reset();

	if (!empty(error)) MessageBoxA(nullptr, error.c_str(), nullptr, MB_OK | MB_ICONERROR);

	return ret;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
	try {
		{
			LPARAM param;
			if (uMsg == WM_MOUSEMOVE && g_app) {
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

		const auto GetDisplayResolutions = [&](bool forceUpdate = false) {
			const auto monitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
			ThrowIfFailed(static_cast<BOOL>(monitor != nullptr));

			if (monitor != s_hMonitor || forceUpdate) {
				ThrowIfFailed(::GetDisplayResolutions(g_displayResolutions, monitor));

				if (const auto resolution = cbegin(g_displayResolutions)->IsPortrait() ? Resolution{ 600, 800 } : Resolution{ 800, 600 };
					*--cend(g_displayResolutions) > resolution) {
					erase_if(g_displayResolutions, [&](const auto& displayResolution) { return displayResolution < resolution; });
				}

				ThrowIfFailed(GetDisplayResolution(g_displayResolution, monitor));

				s_hMonitor = monitor;
			}
		};

		if (s_hMonitor == nullptr) GetDisplayResolutions();

		switch (uMsg) {
			case WM_GETMINMAXINFO:
			{
				if (lParam) {
					const auto style = GetWindowStyle(hWnd), exStyle = GetWindowExStyle(hWnd);
					const auto hasMenu = GetMenu(hWnd) != nullptr;
					const auto DPI = GetDpiForWindow(hWnd);

					const auto AdjustSize = [&](SIZE size, POINT& newSize) {
						RECT rect{ 0, 0, size.cx, size.cy };
						if (AdjustWindowRectExForDpi(&rect, style, hasMenu, exStyle, DPI)) {
							newSize = { rect.right - rect.left, rect.bottom - rect.top };
						}
					};

					auto& minMaxInfo = *reinterpret_cast<PMINMAXINFO>(lParam);
					AdjustSize(*cbegin(g_displayResolutions), minMaxInfo.ptMinTrackSize);
					AdjustSize(g_displayResolution, minMaxInfo.ptMaxTrackSize);
				}
			}
			break;

			case WM_MOVE: GetDisplayResolutions(); break;

			case WM_MOVING:
			case WM_SIZING: g_app->Tick(); break;

			case WM_SIZE:
			{
				if (!g_app) return 0;

				switch (wParam) {
					case SIZE_MINIMIZED: g_app->OnSuspending(); break;

					case SIZE_RESTORED: g_app->OnResuming(); [[fallthrough]];
					default:
					{
						if (g_windowModeHelper->GetMode() != WindowMode::Fullscreen || g_windowModeHelper->IsFullscreenResolutionHandledByWindow()) {
							const Resolution resolution{ LOWORD(lParam), HIWORD(lParam) };

							g_windowModeHelper->SetResolution(resolution);

							g_graphicsSettings.Resolution = resolution;
						}

						g_app->OnWindowSizeChanged();
					}
					break;
				}
			}
			break;

			case WM_DISPLAYCHANGE:
			{
				GetDisplayResolutions(true);

				ThrowIfFailed(g_windowModeHelper->Apply());

				g_app->OnDisplayChanged();
			}
			break;

			case WM_DPICHANGED:
			{
				const auto& [left, top, right, bottom] = *reinterpret_cast<PRECT>(lParam);
				SetWindowPos(hWnd, nullptr, static_cast<int>(left), static_cast<int>(top), static_cast<int>(right - left), static_cast<int>(bottom - top), SWP_NOZORDER);
			}
			break;

			case WM_ACTIVATEAPP:
			{
				if (g_app) {
					if (wParam) g_app->OnActivated();
					else g_app->OnDeactivated();
				}
			}
			[[fallthrough]];
			case WM_ACTIVATE:
			{
				Keyboard::ProcessMessage(uMsg, wParam, lParam);
				Mouse::ProcessMessage(uMsg, wParam, lParam);
			}
			break;

			case WM_SYSKEYDOWN:
			{
				if (wParam == VK_RETURN && (HIWORD(lParam) & (KF_ALTDOWN | KF_REPEAT)) == KF_ALTDOWN) {
					g_windowModeHelper->ToggleMode();
					ThrowIfFailed(g_windowModeHelper->Apply());

					g_graphicsSettings.WindowMode = g_windowModeHelper->GetMode();
				}
			}
			[[fallthrough]];
			case WM_SYSKEYUP:
			case WM_KEYDOWN:
			case WM_KEYUP: Keyboard::ProcessMessage(uMsg, wParam, lParam); break;

			case WM_LBUTTONDOWN:
			case WM_RBUTTONDOWN:
			case WM_MBUTTONDOWN:
			case WM_XBUTTONDOWN:
			{
				SetCapture(hWnd);

				Mouse::ProcessMessage(uMsg, wParam, lParam);
			}
			break;

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
