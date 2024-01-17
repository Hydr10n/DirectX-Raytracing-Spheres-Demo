module;

#include "directxtk12/DDSTextureLoader.h"
#include "directxtk12/DirectXHelpers.h"
#include "directxtk12/WICTextureLoader.h"

#include <filesystem>

export module Texture;

import DescriptorHeap;
import ErrorHelpers;
import TextureHelpers;

using namespace DirectX;
using namespace ErrorHelpers;
using namespace Microsoft::WRL;
using namespace std;
using namespace std::filesystem;
using namespace TextureHelpers;

#define LoadTexture(...) \
	auto isCubeMap = false;\
	ThrowIfFailed(__VA_ARGS__); \
	if (pIsCubeMap != nullptr) *pIsCubeMap = isCubeMap; \
	descriptorHeapIndex = descriptorHeap.Allocate(1, descriptorHeapIndex); \
	DescriptorHeapIndices.SRV = descriptorHeapIndex - 1; \
	CreateShaderResourceView(pDevice, Resource.Get(), descriptorHeap.GetCpuHandle(DescriptorHeapIndices.SRV), isCubeMap);

export {
	enum class TextureType { Unknown, BaseColorMap, EmissiveColorMap, MetallicMap, RoughnessMap, AmbientOcclusionMap, TransmissionMap, OpacityMap, NormalMap, CubeMap };

	struct Texture {
		string Name;

		ComPtr<ID3D12Resource> Resource;

		struct { UINT SRV = ~0u, UAV = ~0u, RTV = ~0u; } DescriptorHeapIndices;

		void Load(const path& filePath, ID3D12Device* pDevice, ResourceUploadBatch& resourceUploadBatch, DescriptorHeapEx& descriptorHeap, _Inout_ UINT& descriptorHeapIndex, bool* pIsCubeMap = nullptr) {
			if (empty(filePath)) throw invalid_argument("Texture file path cannot be empty");

			const auto filePathExtension = filePath.extension();
			if (empty(filePathExtension)) throw invalid_argument(format("{}: Unknown file format", filePath.string()));

			const auto extension = filePathExtension.c_str() + 1;
			LoadTexture(
				!_wcsicmp(extension, L"dds") ? CreateDDSTextureFromFile(pDevice, resourceUploadBatch, filePath.c_str(), &Resource, false, 0, nullptr, &isCubeMap) :
				!_wcsicmp(extension, L"hdr") ? LoadHDR(pDevice, resourceUploadBatch, filePath, &Resource) :
				!_wcsicmp(extension, L"exr") ? LoadEXR(pDevice, resourceUploadBatch, filePath, &Resource) :
				!_wcsicmp(extension, L"tga") ? LoadTGA(pDevice, resourceUploadBatch, filePath, &Resource) :
				CreateWICTextureFromFileEx(pDevice, resourceUploadBatch, filePath.c_str(), 0, D3D12_RESOURCE_FLAG_NONE, WIC_LOADER_FORCE_RGBA32, &Resource),
				filePath.string()
			);
		}

		void Load(string_view format, const void* pData, size_t size, ID3D12Device* pDevice, ResourceUploadBatch& resourceUploadBatch, DescriptorHeapEx& descriptorHeap, _Inout_ UINT& descriptorHeapIndex, bool* pIsCubeMap = nullptr) {
			LoadTexture(
				!_stricmp(data(format), "dds") ? CreateDDSTextureFromMemory(pDevice, resourceUploadBatch, static_cast<const uint8_t*>(pData), size, &Resource, false, 0, nullptr, &isCubeMap) :
				!_stricmp(data(format), "hdr") ? LoadHDR(pDevice, resourceUploadBatch, pData, size, &Resource) :
				!_stricmp(data(format), "tga") ? LoadTGA(pDevice, resourceUploadBatch, pData, size, &Resource) :
				CreateWICTextureFromMemoryEx(pDevice, resourceUploadBatch, static_cast<const uint8_t*>(pData), size, 0, D3D12_RESOURCE_FLAG_NONE, WIC_LOADER_FORCE_RGBA32, &Resource)
			);
		}
	};
}
