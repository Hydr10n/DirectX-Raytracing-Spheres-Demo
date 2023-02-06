module;

#include "pch.h"

#include "directxtk12/DirectXHelpers.h"

#include "directxtk12/DescriptorHeap.h"

#include "directxtk12/ResourceUploadBatch.h"

#include "directxtk12/DDSTextureLoader.h"
#include "directxtk12/WICTextureLoader.h"

#include <map>

#include <semaphore>

#include <filesystem>

export module Texture;

using namespace DirectX;
using namespace DirectX::SimpleMath;
using namespace DX;
using namespace Microsoft::WRL;
using namespace std;
using namespace std::filesystem;

export {
	struct Texture {
		ComPtr<ID3D12Resource> Resource;

		struct { UINT SRV = ~0u, UAV = ~0u, RTV = ~0u; } DescriptorHeapIndices;

		path FilePath;

		void Load(ID3D12Device* pDevice, ResourceUploadBatch& resourceUploadBatch, const DescriptorHeap& descriptorHeap, bool* pIsCubeMap = nullptr, const path& directoryPath = "") {
			bool isCubeMap = false;
			const auto filePath = directoryPath.empty() ? FilePath : directoryPath / FilePath;
			if (const auto filePathString = filePath.string(); !lstrcmpiW(filePath.extension().c_str(), L".dds")) {
				ThrowIfFailed(CreateDDSTextureFromFile(pDevice, resourceUploadBatch, filePath.c_str(), &Resource, false, 0, nullptr, &isCubeMap), filePathString.c_str());
			}
			else ThrowIfFailed(CreateWICTextureFromFileEx(pDevice, resourceUploadBatch, filePath.c_str(), 0, D3D12_RESOURCE_FLAG_NONE, WIC_LOADER_FORCE_RGBA32, &Resource), filePathString.c_str());
			CreateShaderResourceView(pDevice, Resource.Get(), descriptorHeap.GetCpuHandle(DescriptorHeapIndices.SRV), isCubeMap);
			if (pIsCubeMap != nullptr) *pIsCubeMap = isCubeMap;
		}
	};

	enum class TextureType { Unknown, BaseColorMap, EmissiveColorMap, SpecularMap, MetallicMap, RoughnessMap, AmbientOcclusionMap, OpacityMap, NormalMap, CubeMap };

	struct TextureDictionary : map<string, tuple<map<TextureType, Texture>, Matrix /*Transform*/>, less<>> {
		using map::map;

		path DirectoryPath;

		void Load(ID3D12Device* pDevice, ID3D12CommandQueue* pCommandQueue, const DescriptorHeap& descriptorHeap, UINT threadCount = 1) {
			if (!threadCount) throw invalid_argument("Thread count cannot be 0");

			exception_ptr exception;

			vector<thread> threads;

			vector<pair<size_t, const Texture*>> loadedTextures;

			mutex mutex;
			vector<unique_ptr<binary_semaphore>> semaphores;
			for (const auto i : views::iota(0u, threadCount)) semaphores.emplace_back(make_unique<binary_semaphore>(0));

			for (auto& textures : *this | views::values) {
				for (auto& [Type, Texture] : get<0>(textures)) {
					threads.emplace_back(
						[&](size_t threadIndex) {
							try {
								{
									const scoped_lock lock(mutex);

									const decltype(loadedTextures)::value_type* pLoadedTexture = nullptr;
									for (const auto& loadedTexture : loadedTextures) {
										if (loadedTexture.second->FilePath == Texture.FilePath) {
											pLoadedTexture = &loadedTexture;
											break;
										}
									}
									if (pLoadedTexture != nullptr) {
										auto& semaphore = *semaphores.at(pLoadedTexture->first);

										semaphore.acquire();

										Texture = *pLoadedTexture->second;

										semaphore.release();

										return;
									}

									loadedTextures.emplace_back(pair{ threadIndex, &Texture });
								}

								ResourceUploadBatch resourceUploadBatch(pDevice);
								resourceUploadBatch.Begin();

								bool isCubeMap;
								Texture.Load(pDevice, resourceUploadBatch, descriptorHeap, &isCubeMap, DirectoryPath);
								if (isCubeMap != (Type == TextureType::CubeMap)) throw runtime_error(format("{}: Invalid texture", (DirectoryPath / Texture.FilePath).string()));

								resourceUploadBatch.End(pCommandQueue).wait();

								semaphores[threadIndex]->release();
					}
					catch (...) { if (!exception) exception = current_exception(); }
						},
						std::size(threads)
							);

					if (std::size(threads) == threadCount) {
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
