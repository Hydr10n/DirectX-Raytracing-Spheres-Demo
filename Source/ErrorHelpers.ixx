module;

#include <Windows.h>

#include <format>
#include <stacktrace>

export module ErrorHelpers;

export namespace ErrorHelpers {
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
