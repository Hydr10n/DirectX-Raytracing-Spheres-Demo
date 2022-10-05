module;

#include "pch.h"

#include "directxtk12/DirectXHelpers.h"

#include "directxtk12/DescriptorHeap.h"
#include "directxtk12/ResourceUploadBatch.h"

#include "directxtk12/DDSTextureLoader.h"
#include "directxtk12/WICTextureLoader.h"

#include <map>

#include <filesystem>

export module Texture;

using namespace DirectX;
using namespace DX;
using namespace Microsoft::WRL;
using namespace std;
using namespace std::filesystem;

export {
	struct Texture {
		ComPtr<ID3D12Resource> Resource;
		UINT DescriptorHeapIndex = UINT_MAX;
		path FilePath;
	};

	enum class TextureType { Unknown, ColorMap, NormalMap, CubeMap };

	struct TextureDictionary : map<string, tuple<map<TextureType, Texture>, XMMATRIX /*Transform*/>, less<>> {
		using map<key_type, mapped_type, key_compare>::map;

		path DirectoryPath;

		void Load(ID3D12Device* pDevice, ID3D12CommandQueue* pCommandQueue, const DescriptorHeap& descriptorHeap, UINT threadCount = 1) {
			if (!threadCount) throw invalid_argument("Thread count cannot be 0");

			exception_ptr exception;

			vector<thread> threads;
			threads.reserve(threadCount);

			for (auto& [_, textures] : *this) {
				for (auto& [type, texture] : get<0>(textures)) {
					threads.emplace_back([&] {
						try {
							auto& [Resource, DescriptorHeapIndex, FilePath] = texture;

							ResourceUploadBatch resourceUploadBatch(pDevice);
							resourceUploadBatch.Begin();

							bool isCubeMap = false;

							const auto filePath = DirectoryPath / FilePath;
							const auto filePathString = filePath.string();
							if (!lstrcmpiW(filePath.extension().c_str(), L".dds")) {
								ThrowIfFailed(CreateDDSTextureFromFile(pDevice, resourceUploadBatch, filePath.c_str(), &Resource, false, 0, nullptr, &isCubeMap), filePathString.c_str());

								if (isCubeMap != (type == TextureType::CubeMap)) throw runtime_error(filePathString + ": Invalid texture");
							}
							else {
								ThrowIfFailed(CreateWICTextureFromFile(pDevice, resourceUploadBatch, filePath.c_str(), &Resource), filePathString.c_str());
							}

							resourceUploadBatch.End(pCommandQueue).wait();

							CreateShaderResourceView(pDevice, Resource.Get(), descriptorHeap.GetCpuHandle(DescriptorHeapIndex), isCubeMap);
						}
						catch (...) { if (!exception) exception = current_exception(); }
						});

					if (std::size(threads) == threads.capacity()) {
						for (auto& thread : threads) thread.join();
						threads.clear();
					}
				}
			}

			if (!threads.empty()) for (auto& thread : threads) thread.join();

			if (exception) rethrow_exception(exception);
		}
	};
}
