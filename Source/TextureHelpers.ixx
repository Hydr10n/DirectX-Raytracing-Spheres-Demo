module;

#include "directx/d3d12.h"

#include "DirectXTexEXR.h"

#include "directxtk12/ResourceUploadBatch.h"

#include <filesystem>

export module TextureHelpers;

using namespace DirectX;
using namespace std;
using namespace std::filesystem;

#define Load(Loader, ...) \
	HRESULT ret; \
	if (ScratchImage image; SUCCEEDED(ret = Loader(__VA_ARGS__, nullptr, image))) ret = LoadFromMemory(pDevice, resourceUploadBatch, image, ppResource); \
	return ret;

export namespace TextureHelpers {
	HRESULT LoadFromMemory(ID3D12Device* pDevice, ResourceUploadBatch& resourceUploadBatch, const ScratchImage& image, ID3D12Resource** ppResource) {
		HRESULT ret;
		if (vector<D3D12_SUBRESOURCE_DATA> subresources;
			SUCCEEDED(ret = CreateTexture(pDevice, image.GetMetadata(), ppResource))
			&& SUCCEEDED(ret = PrepareUpload(pDevice, image.GetImages(), image.GetImageCount(), image.GetMetadata(), subresources))) {
			resourceUploadBatch.Upload(*ppResource, 0, data(subresources), static_cast<uint32_t>(size(subresources)));
			resourceUploadBatch.Transition(*ppResource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		}
		return ret;
	}

	HRESULT LoadFromHDRMemory(ID3D12Device* pDevice, ResourceUploadBatch& resourceUploadBatch, const void* pData, size_t size, ID3D12Resource** ppResource) { Load(LoadFromHDRMemory, pData, size); }

	HRESULT LoadFromHDRFile(ID3D12Device* pDevice, ResourceUploadBatch& resourceUploadBatch, const path& filePath, ID3D12Resource** ppResource) { Load(LoadFromHDRFile, filePath.c_str()); }

	HRESULT LoadFromEXRFile(ID3D12Device* pDevice, ResourceUploadBatch& resourceUploadBatch, const path& filePath, ID3D12Resource** ppResource) { Load(LoadFromEXRFile, filePath.c_str()); }

	HRESULT LoadFromTGAMemory(ID3D12Device* pDevice, ResourceUploadBatch& resourceUploadBatch, const void* pData, size_t size, ID3D12Resource** ppResource, TGA_FLAGS flags = TGA_FLAGS_NONE) {
		Load(LoadFromTGAMemory, pData, size, flags);
	}

	HRESULT LoadFromTGAFile(ID3D12Device* pDevice, ResourceUploadBatch& resourceUploadBatch, const path& filePath, ID3D12Resource** ppResource, TGA_FLAGS flags = TGA_FLAGS_NONE) {
		Load(LoadFromTGAFile, filePath.c_str(), flags);
	}
}
