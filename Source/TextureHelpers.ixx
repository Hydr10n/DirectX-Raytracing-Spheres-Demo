module;

#include <filesystem>

#include "directx/d3d12.h"

#include "DirectXTexEXR.h"

#include "directxtk12/ResourceUploadBatch.h"

export module TextureHelpers;

using namespace DirectX;
using namespace std;
using namespace std::filesystem;

#define Load(Loader, ...) \
	HRESULT ret; \
	if (ScratchImage image; SUCCEEDED(ret = Loader(__VA_ARGS__, nullptr, image))) ret = LoadTexture(pDevice, resourceUploadBatch, image, ppResource); \
	return ret;

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
}
