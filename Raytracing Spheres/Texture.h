#pragma once

#include "pch.h"

#include "directxtk12/DirectXHelpers.h"

#include "directxtk12/DescriptorHeap.h"
#include "directxtk12/ResourceUploadBatch.h"

#include "directxtk12/DDSTextureLoader.h"
#include "directxtk12/WICTextureLoader.h"

#include <map>

#include <filesystem>

enum class TextureType { ColorMap, NormalMap, CubeMap };

struct Texture {
	UINT DescriptorHeapIndex = UINT_MAX;
	std::wstring Path;
	Microsoft::WRL::ComPtr<ID3D12Resource> Resource;
};

struct TextureDictionary : std::map<std::string, std::tuple<std::map<TextureType, Texture>, DirectX::XMMATRIX /*Transform*/>> {
	using std::map<key_type, mapped_type>::map;

	std::wstring DirectoryPath;

	void Load(
		ID3D12Device* pDevice, ID3D12CommandQueue* pCommandQueue, const DirectX::DescriptorHeap& descriptorHeap,
		UINT threadCount = 1
	) {
		using namespace DirectX;
		using namespace DX;
		using namespace std;
		using namespace std::filesystem;

		if (!threadCount) throw invalid_argument("Thread count cannot be 0");

		exception_ptr exception;

		vector<thread> threads;
		threads.reserve(threadCount);

		for (auto& pair : *this) {
			for (auto& [first, second] : get<0>(pair.second)) {
				threads.emplace_back([&] {
					try {
						auto& [DescriptorHeapIndex, Path, Resource] = second;

						ResourceUploadBatch resourceUploadBatch(pDevice);
						resourceUploadBatch.Begin();

						bool isCubeMap = false;

						const auto filePath = path(DirectoryPath).append(Path);
						const auto filePathString = filePath.string();
						if (!lstrcmpiW(filePath.extension().c_str(), L".dds")) {
							ThrowIfFailed(CreateDDSTextureFromFile(pDevice, resourceUploadBatch, filePath.c_str(), Resource.ReleaseAndGetAddressOf(), false, 0, nullptr, &isCubeMap), filePathString.c_str());

							if ((isCubeMap && first != TextureType::CubeMap) || (!isCubeMap && first == TextureType::CubeMap)) {
								throw runtime_error(filePathString + ": Invalid texture");
							}
						}
						else {
							ThrowIfFailed(CreateWICTextureFromFile(pDevice, resourceUploadBatch, filePath.c_str(), Resource.ReleaseAndGetAddressOf()), filePathString.c_str());
						}

						resourceUploadBatch.End(pCommandQueue).wait();

						CreateShaderResourceView(pDevice, Resource.Get(), descriptorHeap.GetCpuHandle(DescriptorHeapIndex), isCubeMap);
					}
					catch (...) { if (!exception) exception = current_exception(); }
					});

				if (threads.size() == threads.capacity()) {
					for (auto& thread : threads) thread.join();
					threads.clear();
				}
			}
		}

		if (!threads.empty()) for (auto& thread : threads) thread.join();

		if (exception) rethrow_exception(exception);
	}
};
