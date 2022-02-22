#pragma once

#include "WindowBase.h"
#include "WindowHelpers.h"

#include "D3DApp.h"

#include "DisplayHelpers.h"

class MainWindow : public Windows::WindowBase {
public:
	MainWindow() noexcept(false);

	WPARAM Run();

private:
	static constexpr LPCWSTR DefaultTitle = L"Raytracing Spheres";

	WindowHelpers::WindowModeHelper m_windowModeHelper;

	std::unique_ptr<D3DApp> m_app;

	std::vector<DisplayHelpers::Resolution> m_displayResolutions;

	LRESULT CALLBACK OnMessageReceived(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) override;
};
