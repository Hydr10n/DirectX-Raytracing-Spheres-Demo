module;

#include <format>
#include <stacktrace>

#include <Windows.h>

export module ErrorHelpers;

using namespace std;

#define PARAMETERS string_view message = {}, const stacktrace& stacktrace = stacktrace::current()

#define MESSAGE string(message) + (empty(message) ? "" : "\n\n") + to_string(stacktrace)

#define THROW(Type, Succeeded, Code) \
	void ThrowIfFailed(same_as<Type> auto value, PARAMETERS) { \
		if (!Succeeded) { \
			ThrowSystemError(error_code(static_cast<int>(Code), system_category()), message, stacktrace); \
		} \
	}

export namespace ErrorHelpers {
	template <constructible_from<const char*> T>
	[[noreturn]] void Throw(PARAMETERS) { throw T(MESSAGE); }

	[[noreturn]] void ThrowSystemError(error_code code, PARAMETERS) {
		throw system_error(code, format("{}\n\n0x{:08X}", MESSAGE, static_cast<uint32_t>(code.value())));
	}

	THROW(BOOL, value, GetLastError());
	THROW(HRESULT, SUCCEEDED(value), value);
}
