#include "MainWindow.h"

using namespace DirectX;
using namespace DX;
using namespace Microsoft::WRL::Wrappers;
using namespace std;

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPWSTR lpCmdLine, _In_ int nCmdShow) {
	if (!XMVerifyCPUSupport()) {
		MessageBoxA(nullptr, "DirectXMath is not supported", nullptr, MB_OK | MB_ICONERROR);
		return ERROR_CAN_NOT_COMPLETE;
	}

	int ret;
	try {
		RoInitializeWrapper roInitializeWrapper(RO_INIT_MULTITHREADED);
		ThrowIfFailed(roInitializeWrapper);

		ret = static_cast<int>(MainWindow().Run());
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
	return ret;
}
