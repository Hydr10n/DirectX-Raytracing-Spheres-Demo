#include "MainWindow.h"

// Indicate to hybrid graphics systems to prefer the discrete part by default
extern "C" {
	__declspec(dllexport) DWORD NvOptimusEnablement = 1;
	__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPWSTR lpCmdLine, _In_ int nCmdShow) {
	if (!DirectX::XMVerifyCPUSupport()) {
		MessageBoxA(nullptr, "DirectXMath is not supported", nullptr, MB_OK | MB_ICONERROR);
		return ERROR_CAN_NOT_COMPLETE;
	}

	int ret;
	try {
		Microsoft::WRL::Wrappers::RoInitializeWrapper roInitializeWrapper(RO_INIT_MULTITHREADED);
		DX::ThrowIfFailed(roInitializeWrapper);

		ret = static_cast<int>(MainWindow().Run());
	}
	catch (const std::system_error& e) {
		ret = e.code().value();
		MessageBoxA(nullptr, e.what(), nullptr, MB_OK | MB_ICONERROR);
	}
	catch (const std::exception& e) {
		ret = ERROR_CAN_NOT_COMPLETE;
		MessageBoxA(nullptr, e.what(), nullptr, MB_OK | MB_ICONERROR);
	}
	catch (...) {
		ret = static_cast<int>(GetLastError());
		if (ret != ERROR_SUCCESS) MessageBoxA(nullptr, std::system_category().message(ret).c_str(), nullptr, MB_OK | MB_ICONERROR);
	}
	return ret;
}
