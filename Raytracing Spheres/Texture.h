#pragma once

#include <d3d12.h>

#include <DirectXMath.h>

#include <wrl.h>

#include <string>

#include <map>

enum class TextureType { ColorMap, NormalMap, CubeMap };

struct Texture {
	size_t DescriptorHeapIndex = SIZE_MAX;
	std::wstring Path;
	Microsoft::WRL::ComPtr<ID3D12Resource> Resource;
};

using TextureDictionary = std::map<std::string, std::tuple<std::map<TextureType, Texture>, DirectX::XMMATRIX /*Transform*/>>;
