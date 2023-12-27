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

export {
	enum class TextureType { Unknown, BaseColorMap, EmissiveColorMap, MetallicMap, RoughnessMap, AmbientOcclusionMap, TransmissionMap, OpacityMap, NormalMap, CubeMap };

	struct Texture {
		string Name;

		ComPtr<ID3D12Resource> Resource;

		struct { UINT SRV = ~0u, UAV = ~0u, RTV = ~0u; } DescriptorHeapIndices;

		void Load(const path& filePath, ID3D12Device* pDevice, ResourceUploadBatch& resourceUploadBatch, DescriptorHeapEx& descriptorHeap, _Inout_ UINT& descriptorHeapIndex, bool* pIsCubeMap = nullptr) {
			bool isCubeMap = false;
			const auto extension = filePath.extension().c_str() + 1;
			ThrowIfFailed(
				!lstrcmpiW(extension, L"dds") ?
				CreateDDSTextureFromFile(pDevice, resourceUploadBatch, filePath.c_str(), &Resource, false, 0, nullptr, &isCubeMap) :
				!lstrcmpW(extension, L"hdr") ?
				LoadFromHDRFile(pDevice, resourceUploadBatch, filePath, &Resource) :
				!lstrcmpW(extension, L"exr") ?
				LoadFromEXRFile(pDevice, resourceUploadBatch, filePath, &Resource) :
				!lstrcmpW(extension, L"tga") ?
				LoadFromTGAFile(pDevice, resourceUploadBatch, filePath, &Resource) :
				CreateWICTextureFromFileEx(pDevice, resourceUploadBatch, filePath.c_str(), 0, D3D12_RESOURCE_FLAG_NONE, WIC_LOADER_FORCE_RGBA32, &Resource),
				filePath.string()
			);
			if (pIsCubeMap != nullptr) *pIsCubeMap = isCubeMap;

			descriptorHeapIndex = descriptorHeap.Allocate(1, descriptorHeapIndex);
			DescriptorHeapIndices.SRV = descriptorHeapIndex - 1;
			CreateShaderResourceView(pDevice, Resource.Get(), descriptorHeap.GetCpuHandle(DescriptorHeapIndices.SRV), isCubeMap);
		}
	};
}
