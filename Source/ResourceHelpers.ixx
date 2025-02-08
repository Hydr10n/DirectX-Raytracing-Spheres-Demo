module;

#include <filesystem>
#include <latch>
#include <mutex>
#include <ranges>
#include <thread>
#include <unordered_map>
#include <vector>

export module ResourceHelpers;

import ErrorHelpers;

using namespace ErrorHelpers;
using namespace std;
using namespace std::filesystem;

export namespace ResourceHelpers {
	bool AreSamePath(const path& a, const path& b) {
		try {
			return !_wcsicmp(a.c_str(), b.c_str()) || equivalent(a, b);
		}
		catch (...) {
			return false;
		}
	}

	auto ResolveResourcePath(const path& path) {
		return path.is_absolute() ? path : filesystem::path(*__wargv).replace_filename(path);
	}

	template <typename KeyType, typename ResourceType, constructible_from Loader> requires is_class_v<Loader>
	struct ResourceDictionary : unordered_map<KeyType, shared_ptr<ResourceType>> {
		template <typename... Args>
		void Load(const unordered_map<KeyType, path>& descs, bool ignoreLoaded, size_t threadCount, Args&&... args) {
			if (!threadCount) {
				Throw<out_of_range>("Thread count cannot be 0");
			}

			vector<thread> threads;

			struct LoadedResource {
				path FilePath;
				shared_ptr<ResourceType>* Resource;
			};
			vector<pair<size_t, LoadedResource>> loadedResources;

			exception_ptr exception;

			vector<unique_ptr<latch>> latches;
			mutex loadedResourcesMutex, exceptionMutex;

			for (size_t globalThreadIndex = 0; const auto & [URI, FilePath] : descs) {
				latches.emplace_back(make_unique<latch>(1));

				(*this)[URI];
				threads.emplace_back(
					[&, globalThreadIndex](size_t localThreadIndex) {
						try {
					const auto filePath = ResolveResourcePath(FilePath);

					auto& resource = this->at(URI);

					{
						const scoped_lock lock(loadedResourcesMutex);

						if (const auto pLoadedResource = ranges::find_if(loadedResources, [&](const auto& value) { return AreSamePath(value.second.FilePath, filePath); });
							pLoadedResource != ::cend(loadedResources)) {
							if (pLoadedResource->first < localThreadIndex) {
								latches[pLoadedResource->first]->wait();
							}

							resource = *pLoadedResource->second.Resource;

							return;
						}

						loadedResources.emplace_back(globalThreadIndex, LoadedResource{ .FilePath = filePath, .Resource = &resource });
					}

					if (!ignoreLoaded || !resource) {
						resource = make_shared<ResourceType>();
						Loader()(*resource, filePath, forward<Args>(args)...);
					}

					latches[localThreadIndex]->count_down();
				}
				catch (...) {
					const scoped_lock lock(exceptionMutex);

					if (!exception) {
						exception = current_exception();
					}
				}
					},
					size(threads)
						);

				globalThreadIndex++;

				if (size(threads) == threadCount || globalThreadIndex == size(descs)) {
					for (auto& thread : threads) {
						thread.join();
					}
					threads.clear();
					latches.clear();
				}

				if (exception) {
					rethrow_exception(exception);
				}
			}
		}
	};
}
