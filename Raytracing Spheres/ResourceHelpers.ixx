module;

#include <filesystem>

export module ResourceHelpers;

using namespace std;
using namespace std::filesystem;

export namespace ResourceHelpers {
	auto ResolveResourcePath(const path& path) { return path.is_absolute() ? path : filesystem::path(*__wargv).replace_filename(path); }
}
