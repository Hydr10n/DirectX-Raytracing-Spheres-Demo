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

import DeviceContext;
import ErrorHelpers;

using namespace DirectX;
using namespace ErrorHelpers;
using namespace Microsoft::WRL;
using namespace std;
using namespace std::filesystem;

#define LOAD(Loader, ...) \
	HRESULT ret; \
	if (ScratchImage image; SUCCEEDED(ret = Loader(__VA_ARGS__, nullptr, image))) { \
		ret = LoadTexture(deviceContext, resourceUploadBatch, image, ppResource); \
	} \
	return ret;

#define LOAD1(...) \
	auto isCubeMap = false; \
	ComPtr<ID3D12Resource> resource; \
	ThrowIfFailed(__VA_ARGS__); \
	if (pIsCubeMap != nullptr) *pIsCubeMap = isCubeMap; \
	auto texture = make_shared<Texture>(deviceContext, resource.Get(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, false); \
	texture->CreateSRV(isCubeMap); \
	return texture;

export namespace TextureHelpers {
	HRESULT LoadTexture(
		const DeviceContext& deviceContext, ResourceUploadBatch& resourceUploadBatch,
		const ScratchImage& image,
		ID3D12Resource** ppResource
	) {
		HRESULT ret;
		if (vector<D3D12_SUBRESOURCE_DATA> subresourceData;
			SUCCEEDED(ret = CreateTexture(deviceContext.Device, image.GetMetadata(), ppResource))
			&& SUCCEEDED(ret = PrepareUpload(deviceContext.Device, image.GetImages(), image.GetImageCount(), image.GetMetadata(), subresourceData))) {
			resourceUploadBatch.Upload(*ppResource, 0, data(subresourceData), static_cast<uint32_t>(size(subresourceData)));
			resourceUploadBatch.Transition(*ppResource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		}
		return ret;
	}

	HRESULT LoadHDR(
		const DeviceContext& deviceContext, ResourceUploadBatch& resourceUploadBatch,
		const void* pData, size_t size,
		ID3D12Resource** ppResource
	) {
		LOAD(LoadFromHDRMemory, pData, size);
	}

	HRESULT LoadHDR(
		const DeviceContext& deviceContext,
		ResourceUploadBatch& resourceUploadBatch,
		const path& filePath,
		ID3D12Resource** ppResource
	) {
		LOAD(LoadFromHDRFile, filePath.c_str());
	}

	HRESULT LoadEXR(
		const DeviceContext& deviceContext, ResourceUploadBatch& resourceUploadBatch,
		const path& filePath, ID3D12Resource** ppResource
	) {
		LOAD(LoadFromEXRFile, filePath.c_str());
	}

	HRESULT LoadTGA(
		const DeviceContext& deviceContext, ResourceUploadBatch& resourceUploadBatch,
		const void* pData, size_t size,
		ID3D12Resource** ppResource,
		TGA_FLAGS flags = TGA_FLAGS_NONE
	) {
		LOAD(LoadFromTGAMemory, pData, size, flags);
	}

	HRESULT LoadTGA(
		const DeviceContext& deviceContext, ResourceUploadBatch& resourceUploadBatch,
		const path& filePath,
		ID3D12Resource** ppResource,
		TGA_FLAGS flags = TGA_FLAGS_NONE
	) {
		LOAD(LoadFromTGAFile, filePath.c_str(), flags);
	}

	shared_ptr<Texture> LoadTexture(
		const path& filePath,
		const DeviceContext& deviceContext, ResourceUploadBatch& resourceUploadBatch,
		bool* pIsCubeMap = nullptr
	) {
		if (empty(filePath)) throw invalid_argument("Texture file path cannot be empty");

		const auto filePathExtension = filePath.extension();
		if (empty(filePathExtension)) throw invalid_argument(format("{}: Unknown file format", filePath.string()));

		const auto extension = filePathExtension.c_str() + 1;
		LOAD1(
			!_wcsicmp(extension, L"dds") ?
			CreateDDSTextureFromFile(deviceContext.Device, resourceUploadBatch, filePath.c_str(), &resource, false, 0, nullptr, &isCubeMap) :
			!_wcsicmp(extension, L"hdr") ?
			LoadHDR(deviceContext, resourceUploadBatch, filePath, &resource) :
			!_wcsicmp(extension, L"exr") ?
			LoadEXR(deviceContext, resourceUploadBatch, filePath, &resource) :
			!_wcsicmp(extension, L"tga") ?
			LoadTGA(deviceContext, resourceUploadBatch, filePath, &resource) :
			CreateWICTextureFromFileEx(deviceContext.Device, resourceUploadBatch, filePath.c_str(), 0, D3D12_RESOURCE_FLAG_NONE, WIC_LOADER_FORCE_RGBA32, &resource),
			filePath.string()
		);
	}

	shared_ptr<Texture> LoadTexture(
		string_view format, const void* pData, size_t size,
		const DeviceContext& deviceContext, ResourceUploadBatch& resourceUploadBatch,
		bool* pIsCubeMap = nullptr
	) {
		LOAD1(
			!_stricmp(data(format), "dds") ?
			CreateDDSTextureFromMemory(deviceContext.Device, resourceUploadBatch, static_cast<const uint8_t*>(pData), size, &resource, false, 0, nullptr, &isCubeMap) :
			!_stricmp(data(format), "hdr") ?
			LoadHDR(deviceContext, resourceUploadBatch, pData, size, &resource) :
			!_stricmp(data(format), "tga") ?
			LoadTGA(deviceContext, resourceUploadBatch, pData, size, &resource) :
			CreateWICTextureFromMemoryEx(deviceContext.Device, resourceUploadBatch, static_cast<const uint8_t*>(pData), size, 0, D3D12_RESOURCE_FLAG_NONE, WIC_LOADER_FORCE_RGBA32, &resource)
		);
	}
}
