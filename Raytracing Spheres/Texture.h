#pragma once

#include <d3d12.h>

#include <wrl.h>

#include <string>

struct Texture {
	size_t DescriptorHeapIndex = SIZE_MAX;
	std::wstring Path;
	Microsoft::WRL::ComPtr<ID3D12Resource> Resource;
};
