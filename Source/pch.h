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

#include <wrl.h>

#include "directxtk12/SimpleMath.h"
#include <DirectXColors.h>

#include <numbers>

#include <format>

#include <span>

#include <ranges>

#include <stacktrace>

// To use graphics and CPU markup events with the latest version of PIX, change this to include <pix3.h>
// then add the NuGet package WinPixEventRuntime to the project.
#include <pix.h>

namespace DX {
	[[noreturn]] inline void throw_std_system_error(int code, const std::string& message = "", const std::stacktrace& stacktrace = std::stacktrace::current()) {
		throw std::system_error(code, std::system_category(), std::format("{}{}{}", message, std::empty(message) ? "" : "\n\n", std::to_string(stacktrace)));
	}

	inline void ThrowIfFailed(BOOL value, const std::string& message = "", const std::stacktrace& stacktrace = std::stacktrace::current()) {
		if (!value) throw_std_system_error(static_cast<int>(GetLastError()), message, stacktrace);
	}

	inline void ThrowIfFailed(HRESULT value, const std::string& message = "", const std::stacktrace& stacktrace = std::stacktrace::current()) {
		if (FAILED(value)) throw_std_system_error(static_cast<int>(value), message, stacktrace);
	}
}
