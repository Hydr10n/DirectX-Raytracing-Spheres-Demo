//
// pch.h
// Header for standard system include files.
//

#pragma once

#include <winsdkver.h>
#define _WIN32_WINNT 0x0A00
#include <sdkddkver.h>

#include <windowsx.h>

// Use the C++ standard templated min/max
#define NOMINMAX

#define WIN32_LEAN_AND_MEAN

#include "d3dx12.h"
#include <dxgi1_6.h>

#include <DirectXCollision.h>
#include <DirectXColors.h>

#include <wrl.h>

#include <algorithm>
#include <memory>

// To use graphics and CPU markup events with the latest version of PIX, change this to include <pix3.h>
// then add the NuGet package WinPixEventRuntime to the project.
#include <pix.h>

#ifdef _DEBUG
#include <dxgidebug.h>
#endif

#include "ErrorHelpers.h"
namespace DX { using namespace ErrorHelpers; }
