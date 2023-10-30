module;

#include "directx/d3d12.h"

#include "DirectXTexEXR.h"

#include "directxtk12/ResourceUploadBatch.h"

export module TextureHelpers;

using namespace DirectX;
using namespace std;

#define Load(Loader, ...) \
	HRESULT ret; \
	if (ScratchImage image; SUCCEEDED(ret = Loader(__VA_ARGS__, nullptr, image))) ret = LoadFromMemory(pDevice, resourceUploadBatch, image, ppResource, generateMipsIfMissing); \
	return ret;

export namespace TextureHelpers {
	HRESULT LoadFromMemory(
		ID3D12Device* pDevice, ResourceUploadBatch& resourceUploadBatch, const ScratchImage& image, ID3D12Resource** ppResource, bool generateMipsIfMissing = false
	) {
		HRESULT ret;
		if (vector<D3D12_SUBRESOURCE_DATA> subresources;
			SUCCEEDED(ret = CreateTexture(pDevice, image.GetMetadata(), ppResource))
			&& SUCCEEDED(ret = PrepareUpload(pDevice, image.GetImages(), image.GetImageCount(), image.GetMetadata(), subresources))) {
			resourceUploadBatch.Upload(*ppResource, 0, data(subresources), static_cast<uint32_t>(size(subresources)));
			resourceUploadBatch.Transition(*ppResource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			if (generateMipsIfMissing) {
				if (const auto resourceDesc = (*ppResource)->GetDesc(); resourceUploadBatch.IsSupportedForGenerateMips(resourceDesc.Format) && size(subresources) != resourceDesc.MipLevels) {
					resourceUploadBatch.GenerateMips(*ppResource);
				}
			}
		}
		return ret;
	}

	HRESULT LoadFromHDRMemory(
		ID3D12Device* pDevice, ResourceUploadBatch& resourceUploadBatch, const void* pData, size_t size, ID3D12Resource** ppResource,
		bool generateMipsIfMissing = false
	) {
		Load(LoadFromHDRMemory, pData, size);
	}

	HRESULT LoadFromHDRFile(
		ID3D12Device* pDevice, ResourceUploadBatch& resourceUploadBatch, LPCWSTR fileName, ID3D12Resource** ppResource,
		bool generateMipsIfMissing = false
	) {
		Load(LoadFromHDRFile, fileName);
	}

	HRESULT LoadFromEXRFile(
		ID3D12Device* pDevice, ResourceUploadBatch& resourceUploadBatch, LPCWSTR fileName, ID3D12Resource** ppResource,
		bool generateMipsIfMissing = false
	) {
		Load(LoadFromEXRFile, fileName);
	}

	HRESULT LoadFromTGAMemory(
		ID3D12Device* pDevice, ResourceUploadBatch& resourceUploadBatch, const void* pData, size_t size, ID3D12Resource** ppResource,
		bool generateMipsIfMissing = false, TGA_FLAGS flags = TGA_FLAGS_NONE
	) {
		Load(LoadFromTGAMemory, pData, size, flags);
	}

	HRESULT LoadFromTGAFile(
		ID3D12Device* pDevice, ResourceUploadBatch& resourceUploadBatch, LPCWSTR fileName, ID3D12Resource** ppResource,
		bool generateMipsIfMissing = false, TGA_FLAGS flags = TGA_FLAGS_NONE
	) {
		Load(LoadFromTGAFile, fileName, flags);
	}
}
