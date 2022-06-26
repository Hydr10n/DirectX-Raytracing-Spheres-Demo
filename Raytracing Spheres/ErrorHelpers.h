#pragma once

#include <Windows.h>

#include <system_error>

namespace ErrorHelpers {
	[[noreturn]] void throw_std_system_error(std::integral auto code, const char* message = "") {
		throw std::system_error(static_cast<int>(code), std::system_category(), message);
	}

	inline void ThrowIfFailed(BOOL value, LPCSTR lpMessage = "") { if (!value) throw_std_system_error(GetLastError(), lpMessage); }

	inline void ThrowIfFailed(HRESULT value, LPCSTR lpMessage = "") { if (FAILED(value)) throw_std_system_error(value, lpMessage); }
}
