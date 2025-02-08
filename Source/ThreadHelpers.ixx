module;

#include <future>

export module ThreadHelpers;

using namespace std;

export namespace ThreadHelpers {
	template <typename Function, typename... Args>
	auto StartDetachedFuture(Function&& function, Args&&... args) {
		using ResultType = invoke_result_t<Function, Args...>;
		promise<ResultType> promise;
		auto future = promise.get_future();
		thread([function, ...args = forward<Args>(args), promise = move(promise)]() mutable {
			if constexpr (is_same_v<ResultType, void>) {
				function(forward<Args>(args)...);
				promise.set_value();
			}
			else {
				promise.set_value(function(forward<Args>(args)...));
			}
		}).detach();
		return future;
	}
}
