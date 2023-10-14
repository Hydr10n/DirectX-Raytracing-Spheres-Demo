module;

#include "directx/d3dx12.h"

#include "DirectXTexEXR.h"

#include "directxtk12/ResourceUploadBatch.h"

export module TextureHelpers;

using namespace DirectX;
using namespace std;

#define Load(Loader, ...) \
	HRESULT ret; \
	if (ScratchImage image; SUCCEEDED(ret = Loader(__VA_ARGS__, nullptr, image))) ret = LoadFromMemory(pDevice, resourceUploadBatch, image, ppResource, generateMips); \
	return ret;

export namespace TextureHelpers {
	HRESULT LoadFromMemory(ID3D12Device* pDevice, ResourceUploadBatch& resourceUploadBatch, const ScratchImage& image, ID3D12Resource** ppResource, bool generateMips = false) {
		HRESULT ret;
		if (vector<D3D12_SUBRESOURCE_DATA> subresources;
			SUCCEEDED(ret = CreateTexture(pDevice, image.GetMetadata(), ppResource))
			&& SUCCEEDED(ret = PrepareUpload(pDevice, image.GetImages(), image.GetImageCount(), image.GetMetadata(), subresources))) {
			resourceUploadBatch.Upload(*ppResource, 0, data(subresources), static_cast<uint32_t>(size(subresources)));
			resourceUploadBatch.Transition(*ppResource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			if (generateMips) resourceUploadBatch.GenerateMips(*ppResource);
		}
		return ret;
	}

	HRESULT LoadFromHDRMemory(
		ID3D12Device* pDevice, ResourceUploadBatch& resourceUploadBatch, const void* pData, size_t size, ID3D12Resource** ppResource,
		bool generateMips = false
	) {
		Load(LoadFromHDRMemory, pData, size);
	}

	HRESULT LoadFromHDRFile(
		ID3D12Device* pDevice, ResourceUploadBatch& resourceUploadBatch, LPCWSTR fileName, ID3D12Resource** ppResource,
		bool generateMips = false
	) {
		Load(LoadFromHDRFile, fileName);
	}

	HRESULT LoadFromEXRFile(
		ID3D12Device* pDevice, ResourceUploadBatch& resourceUploadBatch, LPCWSTR fileName, ID3D12Resource** ppResource,
		bool generateMips = false
	) {
		Load(LoadFromEXRFile, fileName);
	}

	HRESULT LoadFromTGAMemory(
		ID3D12Device* pDevice, ResourceUploadBatch& resourceUploadBatch, const void* pData, size_t size, ID3D12Resource** ppResource,
		bool generateMips = false, TGA_FLAGS flags = TGA_FLAGS_NONE
	) {
		Load(LoadFromTGAMemory, pData, size, flags);
	}

	HRESULT LoadFromTGAFile(
		ID3D12Device* pDevice, ResourceUploadBatch& resourceUploadBatch, LPCWSTR fileName, ID3D12Resource** ppResource,
		bool generateMips = false, TGA_FLAGS flags = TGA_FLAGS_NONE
	) {
		Load(LoadFromTGAFile, fileName, flags);
	}
}
