module;

#include "pch.h"

#include "directxtk12/DirectXHelpers.h"

#include "directxtk12/DescriptorHeap.h"

#include "directxtk12/ResourceUploadBatch.h"

#include "directxtk12/DDSTextureLoader.h"
#include "directxtk12/WICTextureLoader.h"

#include <filesystem>

export module Texture;

import DirectX.DescriptorHeap;

using namespace DirectX;
using namespace DirectX::SimpleMath;
using namespace DX;
using namespace Microsoft::WRL;
using namespace std;
using namespace std::filesystem;

export {
	enum class TextureType { Unknown, BaseColorMap, EmissiveColorMap, MetallicMap, RoughnessMap, AmbientOcclusionMap, TransmissionMap, OpacityMap, NormalMap, CubeMap };

	struct Texture {
		string Name;

		ComPtr<ID3D12Resource> Resource;

		struct { UINT SRV = ~0u, UAV = ~0u, RTV = ~0u; } DescriptorHeapIndices;

		void Load(const path& filePath, ID3D12Device* pDevice, ResourceUploadBatch& resourceUploadBatch, const DescriptorHeap& descriptorHeap, bool* pIsCubeMap = nullptr) {
			bool isCubeMap = false;
			if (const auto ret = !lstrcmpiW(filePath.extension().c_str(), L".dds") ?
				CreateDDSTextureFromFile(pDevice, resourceUploadBatch, filePath.c_str(), &Resource, false, 0, nullptr, &isCubeMap) :
				CreateWICTextureFromFileEx(pDevice, resourceUploadBatch, filePath.c_str(), 0, D3D12_RESOURCE_FLAG_NONE, WIC_LOADER_FORCE_RGBA32, &Resource);
				FAILED(ret)) {
				throw_std_system_error(ret, filePath.string());
			}
			CreateShaderResourceView(pDevice, Resource.Get(), descriptorHeap.GetCpuHandle(DescriptorHeapIndices.SRV), isCubeMap);
			if (pIsCubeMap != nullptr) *pIsCubeMap = isCubeMap;
		}

		void Load(const path& filePath, ID3D12Device* pDevice, ResourceUploadBatch& resourceUploadBatch, DescriptorHeapEx& descriptorHeap, _Inout_ UINT& descriptorHeapIndex, bool* pIsCubeMap = nullptr) {
			descriptorHeapIndex = descriptorHeap.Allocate(1, descriptorHeapIndex);
			DescriptorHeapIndices.SRV = descriptorHeapIndex - 1;
			Load(filePath, pDevice, resourceUploadBatch, descriptorHeap, pIsCubeMap);
		}
	};
}
