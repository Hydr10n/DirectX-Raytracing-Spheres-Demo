module;
#include <Windows.h>

#include <memory>

export module D3DApp;

import WindowHelpers;

using namespace WindowHelpers;

export struct D3DApp {
	D3DApp(const std::shared_ptr<WindowModeHelper>& windowModeHelper) noexcept(false);
	~D3DApp();

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
	const std::unique_ptr<Impl> m_impl;
};
