module;

#include <Windows.h>

#include <format>
#include <stacktrace>

export module ErrorHelpers;

using namespace std;

#define Throw(Type, Succeeded) \
	void ThrowIfFailed(same_as<Type> auto value, const string& message = "", const stacktrace& stacktrace = stacktrace::current()) { \
		if (!Succeeded) throw_std_system_error(static_cast<int>(GetLastError()), message, stacktrace); \
	}

export namespace ErrorHelpers {
	template <derived_from<exception> T> requires requires { T(""); }
	[[noreturn]] void throw_std_exception(const string& message, const stacktrace& stacktrace = stacktrace::current()) {
		throw T(format("{}{}{}", message, empty(message) ? "" : "\n\n", to_string(stacktrace)));
	}

	[[noreturn]] void throw_std_system_error(int code, const string& message = "", const stacktrace& stacktrace = stacktrace::current()) {
		throw system_error(code, system_category(), format("{}{}{}", message, empty(message) ? "" : "\n\n", to_string(stacktrace)));
	}

	Throw(BOOL, value);
	Throw(HRESULT, SUCCEEDED(value));
}
