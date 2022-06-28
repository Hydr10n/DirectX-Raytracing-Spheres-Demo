//
// pch.h
// Header for standard system include files.
//

#pragma once

#include <winsdkver.h>
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <sdkddkver.h>

// Use the C++ standard templated min/max
#define NOMINMAX

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <windowsx.h>

#include "directx/d3dx12.h"
#include <dxgi1_6.h>

#ifdef _DEBUG
#include <dxgidebug.h>
#endif

#include <DirectXCollision.h>
#include <DirectXColors.h>

#include <wrl.h>

#include <algorithm>

#include <system_error>

// To use graphics and CPU markup events with the latest version of PIX, change this to include <pix3.h>
// then add the NuGet package WinPixEventRuntime to the project.
#include <pix.h>

namespace DX {
	[[noreturn]] inline void throw_std_system_error(int code, const char* message = "") {
		throw std::system_error(code, std::system_category(), message);
	}

	inline void ThrowIfFailed(std::same_as<BOOL> auto value, LPCSTR lpMessage = "") {
		if (!value) throw_std_system_error(static_cast<int>(GetLastError()), lpMessage);
	}

	inline void ThrowIfFailed(std::same_as<HRESULT> auto value, LPCSTR lpMessage = "") {
		if (FAILED(value)) throw_std_system_error(static_cast<int>(value), lpMessage);
	}
}
