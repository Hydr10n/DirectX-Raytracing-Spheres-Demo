#include "D3DApp.h"

#include "SharedData.h"
#include "MyAppData.h"

#include "imgui_impl_win32.h"

#include "resource.h"

using namespace DirectX;
using namespace DisplayHelpers;
using namespace DX;
using namespace Microsoft::WRL::Wrappers;
using namespace std;
using namespace WindowHelpers;

using SettingsData = MyAppData::Settings;
using SettingsKeys = SettingsData::Keys;

constexpr auto DefaultWindowTitle = L"Raytracing Spheres";

exception_ptr g_exception;

unique_ptr<D3DApp> g_app;

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

int WINAPI wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPWSTR lpCmdLine, _In_ int nShowCmd) {
	UNREFERENCED_PARAMETER(lpCmdLine);
	UNREFERENCED_PARAMETER(nShowCmd);

	if (!XMVerifyCPUSupport()) {
		MessageBoxW(nullptr, L"DirectXMath is not supported.", nullptr, MB_OK | MB_ICONERROR);
		return ERROR_CAN_NOT_COMPLETE;
	}

	int ret;

	RoInitializeWrapper roInitializeWrapper(RO_INIT_MULTITHREADED);

	try {
		ThrowIfFailed(roInitializeWrapper);

		ThrowIfFailed(GetDisplayResolutions(g_displayResolutions));

		const WNDCLASSEXW wndClassEx{
			.cbSize = sizeof(wndClassEx),
			.lpfnWndProc = WndProc,
			.hInstance = hInstance,
			.hIcon = LoadIcon(GetModuleHandle(nullptr), MAKEINTRESOURCE(IDI_ICON_DIRECTX)),
			.hCursor = LoadCursor(nullptr, IDC_ARROW),
			.lpszClassName = L"Direct3D 12"
		};
		ThrowIfFailed(RegisterClassExW(&wndClassEx));

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
		ThrowIfFailed(window != nullptr);

		g_windowModeHelper = make_unique<decltype(g_windowModeHelper)::element_type>(window);

		g_windowModeHelper->WindowedStyle = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;

		if (!SettingsData::Load<SettingsKeys::Resolution>(g_windowModeHelper->Resolution)) {
			RECT rect;
			ThrowIfFailed(GetClientRect(g_windowModeHelper->hWnd, &rect));
			g_windowModeHelper->Resolution = { rect.right - rect.left, rect.bottom - rect.top };
		}
		else if (!g_displayResolutions.contains(g_windowModeHelper->Resolution)) {
			g_windowModeHelper->Resolution = *--g_displayResolutions.cend();
		}

		g_app = make_unique<decltype(g_app)::element_type>();

		ThrowIfFailed(g_windowModeHelper->Apply()); // Fix missing icon on title bar when initial WindowMode != Windowed

		WindowMode windowMode;
		if (SettingsData::Load<SettingsData::Keys::WindowMode>(windowMode)) {
			g_windowModeHelper->SetMode(windowMode);
			ThrowIfFailed(g_windowModeHelper->Apply());
		}

		MSG msg;
		msg.message = WM_QUIT;
		do {
			if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
				TranslateMessage(&msg);
				DispatchMessageW(&msg);

				if (g_exception != nullptr) rethrow_exception(g_exception);
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
			auto param = lParam;

			if (uMsg == WM_MOUSEMOVE) {
				const auto outputSize = g_app->GetOutputSize();
				RECT rect;
				GetClientRect(hWnd, &rect);
				param = MAKELPARAM(GET_X_LPARAM(lParam) * outputSize.cx / static_cast<float>(rect.right - rect.left), GET_Y_LPARAM(lParam) * outputSize.cy / static_cast<float>(rect.bottom - rect.top));
			}

			extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
			if (const auto ret = ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, param)) return ret;
		}

		switch (uMsg) {
		case WM_GETMINMAXINFO: {
			if (lParam) {
				auto& minMaxInfo = *reinterpret_cast<PMINMAXINFO>(lParam);

				const auto style = GetWindowStyle(hWnd), exStyle = GetWindowExStyle(hWnd);
				const auto hasMenu = GetMenu(hWnd) != nullptr;
				const auto DPI = GetDpiForWindow(hWnd);

				RECT rect;

				rect = { 0, 0, 320, 200 };
				if (AdjustWindowRectExForDpi(&rect, style, hasMenu, exStyle, DPI)) {
					minMaxInfo.ptMinTrackSize = { rect.right - rect.left, rect.bottom - rect.top };
				}

				const auto& displayResolution = *--g_displayResolutions.cend();
				rect = { 0, 0, displayResolution.cx, displayResolution.cy };
				if (AdjustWindowRectExForDpi(&rect, style, hasMenu, exStyle, DPI)) {
					minMaxInfo.ptMaxTrackSize = { rect.right - rect.left, rect.bottom - rect.top };
				}
			}
		}	break;

		case WM_MOVING:
		case WM_SIZING: g_app->Tick(); break;

		case WM_SIZE: {
			switch (wParam) {
			case SIZE_MINIMIZED: g_app->OnSuspending(); break;

			case SIZE_RESTORED: g_app->OnResuming(); [[fallthrough]];
			default: g_app->OnWindowSizeChanged(); break;
			}
		}	break;

		case WM_DPICHANGED: {
			const auto& rect = *reinterpret_cast<PRECT>(lParam);
			SetWindowPos(hWnd, nullptr, static_cast<int>(rect.left), static_cast<int>(rect.top), static_cast<int>(rect.right - rect.left), static_cast<int>(rect.bottom - rect.top), SWP_NOZORDER);
		}	break;

		case WM_SYSKEYDOWN: {
			if (wParam == VK_RETURN && (lParam & 0x60000000) == 0x20000000) {
				g_windowModeHelper->ToggleMode();
				ThrowIfFailed(g_windowModeHelper->Apply());

				SettingsData::Save<SettingsKeys::WindowMode>(g_windowModeHelper->GetMode());
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
		}	break;

		case WM_LBUTTONUP:
		case WM_RBUTTONUP:
		case WM_MBUTTONUP:
		case WM_XBUTTONUP: ReleaseCapture(); [[fallthrough]];
		case WM_INPUT:
		case WM_MOUSEMOVE:
		case WM_MOUSEWHEEL:
		case WM_MOUSEHOVER: Mouse::ProcessMessage(uMsg, wParam, lParam); break;

		case WM_ACTIVATEAPP: {
			if (wParam) {
				g_app->OnActivated();

				SetForegroundWindow(hWnd);
			}
			else g_app->OnDeactivated();
		} [[fallthrough]];
		case WM_ACTIVATE: {
			Keyboard::ProcessMessage(uMsg, wParam, lParam);
			Mouse::ProcessMessage(uMsg, wParam, lParam);
		}	break;

			//case WM_MOUSEACTIVATE: return MA_ACTIVATEANDEAT;

		case WM_MENUCHAR: return MAKELRESULT(0, MNC_CLOSE);

		case WM_DESTROY: PostQuitMessage(ERROR_SUCCESS); break;

		default: return DefWindowProcW(hWnd, uMsg, wParam, lParam);
		}
	}
	catch (...) { g_exception = current_exception(); }

	return 0;
}
