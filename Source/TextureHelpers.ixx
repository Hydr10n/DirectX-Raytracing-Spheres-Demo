module;

#include <filesystem>

#include <wrl.h>

#include "directx/d3d12.h"

#include "DirectXTexEXR.h"

#include "directxtk12/DDSTextureLoader.h"
#include "directxtk12/ResourceUploadBatch.h"
#include "directxtk12/WICTextureLoader.h"

export module TextureHelpers;

export import Texture;

import DescriptorHeap;
import ErrorHelpers;

using namespace DirectX;
using namespace ErrorHelpers;
using namespace Microsoft::WRL;
using namespace std;
using namespace std::filesystem;

#define Load(Loader, ...) \
	HRESULT ret; \
	if (ScratchImage image; SUCCEEDED(ret = Loader(__VA_ARGS__, nullptr, image))) ret = LoadTexture(pDevice, resourceUploadBatch, image, ppResource); \
	return ret;

#define Load1(...) \
	auto isCubeMap = false;\
	ComPtr<ID3D12Resource> resource; \
	ThrowIfFailed(__VA_ARGS__); \
	descriptorIndex = descriptorHeap.Allocate(1, descriptorIndex); \
	if (pIsCubeMap != nullptr) *pIsCubeMap = isCubeMap; \
	auto texture = make_shared<Texture>(resource.Get(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE); \
	texture->CreateSRV(descriptorHeap, descriptorIndex - 1, isCubeMap); \
	return texture;

export namespace TextureHelpers {
	HRESULT LoadTexture(ID3D12Device* pDevice, ResourceUploadBatch& resourceUploadBatch, const ScratchImage& image, ID3D12Resource** ppResource) {
		HRESULT ret;
		if (vector<D3D12_SUBRESOURCE_DATA> subresources;
			SUCCEEDED(ret = CreateTexture(pDevice, image.GetMetadata(), ppResource))
			&& SUCCEEDED(ret = PrepareUpload(pDevice, image.GetImages(), image.GetImageCount(), image.GetMetadata(), subresources))) {
			resourceUploadBatch.Upload(*ppResource, 0, data(subresources), static_cast<uint32_t>(size(subresources)));
			resourceUploadBatch.Transition(*ppResource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		}
		return ret;
	}

	HRESULT LoadHDR(ID3D12Device* pDevice, ResourceUploadBatch& resourceUploadBatch, const void* pData, size_t size, ID3D12Resource** ppResource) { Load(LoadFromHDRMemory, pData, size); }

	HRESULT LoadHDR(ID3D12Device* pDevice, ResourceUploadBatch& resourceUploadBatch, const path& filePath, ID3D12Resource** ppResource) { Load(LoadFromHDRFile, filePath.c_str()); }

	HRESULT LoadEXR(ID3D12Device* pDevice, ResourceUploadBatch& resourceUploadBatch, const path& filePath, ID3D12Resource** ppResource) { Load(LoadFromEXRFile, filePath.c_str()); }

	HRESULT LoadTGA(ID3D12Device* pDevice, ResourceUploadBatch& resourceUploadBatch, const void* pData, size_t size, ID3D12Resource** ppResource, TGA_FLAGS flags = TGA_FLAGS_NONE) {
		Load(LoadFromTGAMemory, pData, size, flags);
	}

	HRESULT LoadTGA(ID3D12Device* pDevice, ResourceUploadBatch& resourceUploadBatch, const path& filePath, ID3D12Resource** ppResource, TGA_FLAGS flags = TGA_FLAGS_NONE) {
		Load(LoadFromTGAFile, filePath.c_str(), flags);
	}

	shared_ptr<Texture> LoadTexture(const path& filePath, ID3D12Device* pDevice, ResourceUploadBatch& resourceUploadBatch, DescriptorHeapEx& descriptorHeap, _Inout_ UINT& descriptorIndex, bool* pIsCubeMap = nullptr) {
		if (empty(filePath)) throw invalid_argument("Texture file path cannot be empty");

		const auto filePathExtension = filePath.extension();
		if (empty(filePathExtension)) throw invalid_argument(format("{}: Unknown file format", filePath.string()));

		const auto extension = filePathExtension.c_str() + 1;
		Load1(
			!_wcsicmp(extension, L"dds") ? CreateDDSTextureFromFile(pDevice, resourceUploadBatch, filePath.c_str(), &resource, false, 0, nullptr, &isCubeMap) :
			!_wcsicmp(extension, L"hdr") ? LoadHDR(pDevice, resourceUploadBatch, filePath, &resource) :
			!_wcsicmp(extension, L"exr") ? LoadEXR(pDevice, resourceUploadBatch, filePath, &resource) :
			!_wcsicmp(extension, L"tga") ? LoadTGA(pDevice, resourceUploadBatch, filePath, &resource) :
			CreateWICTextureFromFileEx(pDevice, resourceUploadBatch, filePath.c_str(), 0, D3D12_RESOURCE_FLAG_NONE, WIC_LOADER_FORCE_RGBA32, &resource),
			filePath.string()
		);
	}

	shared_ptr<Texture> LoadTexture(string_view format, const void* pData, size_t size, ID3D12Device* pDevice, ResourceUploadBatch& resourceUploadBatch, DescriptorHeapEx& descriptorHeap, _Inout_ UINT& descriptorIndex, bool* pIsCubeMap = nullptr) {
		Load1(
			!_stricmp(data(format), "dds") ? CreateDDSTextureFromMemory(pDevice, resourceUploadBatch, static_cast<const uint8_t*>(pData), size, &resource, false, 0, nullptr, &isCubeMap) :
			!_stricmp(data(format), "hdr") ? LoadHDR(pDevice, resourceUploadBatch, pData, size, &resource) :
			!_stricmp(data(format), "tga") ? LoadTGA(pDevice, resourceUploadBatch, pData, size, &resource) :
			CreateWICTextureFromMemoryEx(pDevice, resourceUploadBatch, static_cast<const uint8_t*>(pData), size, 0, D3D12_RESOURCE_FLAG_NONE, WIC_LOADER_FORCE_RGBA32, &resource)
		);
	}
}
