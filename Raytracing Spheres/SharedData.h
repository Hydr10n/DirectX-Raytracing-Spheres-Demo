#pragma once

#include "WindowHelpers.h"

#include <memory>

inline DisplayHelpers::ResolutionSet g_displayResolutions;

inline std::unique_ptr<WindowHelpers::WindowModeHelper> g_windowModeHelper;
