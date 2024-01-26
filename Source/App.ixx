module;

#include <Windows.h>

#include <memory>

export module App;

import WindowHelpers;

using namespace WindowHelpers;
using namespace std;

export struct App {
	explicit App(WindowModeHelper& windowModeHelper) noexcept(false);
	~App();

	SIZE GetOutputSize() const noexcept;

	void Tick();

	void OnWindowSizeChanged();

	void OnDisplayChanged();

	void OnResuming();
	void OnSuspending();

	void OnActivated();
	void OnDeactivated();

private:
	struct Impl;
	const unique_ptr<Impl> m_impl;
};
