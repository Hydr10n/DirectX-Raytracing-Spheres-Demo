module;

#include <functional>
#include <unordered_map>

export module Event;

using namespace std;

export {
	using EventHandle = size_t;

	template <typename... Args>
	class Event {
	public:
		[[nodiscard]] EventHandle operator+=(function<void(Args...)> handler) {
			m_handlers[m_count] = handler;
			return m_count++;
		}

		Event& operator-=(EventHandle handle) {
			m_handlers.erase(handle);
			return *this;
		}

		void Raise(Args&&... args) const {
			for (const auto& [_, Handler] : m_handlers) Handler(forward<Args>(args)...);
		}

	private:
		EventHandle m_count{};
		unordered_map<EventHandle, function<void(Args...)>> m_handlers;
	};
}
