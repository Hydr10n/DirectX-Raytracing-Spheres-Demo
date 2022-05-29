#pragma once

#include "pch.h"

#include "DirectXTK12/DirectXHelpers.h"

#include "DirectXTK12/DescriptorHeap.h"
#include "DirectXTK12/ResourceUploadBatch.h"

#include "DirectXTK12/DDSTextureLoader.h"
#include "DirectXTK12/WICTextureLoader.h"

#include <map>

#include <filesystem>

enum class TextureType { ColorMap, NormalMap, CubeMap };

struct Texture {
	size_t DescriptorHeapIndex = SIZE_MAX;
	std::wstring Path;
	Microsoft::WRL::ComPtr<ID3D12Resource> Resource;
};

struct TextureDictionary : std::map<std::string, std::tuple<std::map<TextureType, Texture>, DirectX::XMMATRIX /*Transform*/>> {
	using std::map<key_type, mapped_type>::map;

	std::wstring DirectoryPath;

	void Load(ID3D12Device* pDevice, ID3D12CommandQueue* pCommandQueue, const DirectX::DescriptorHeap& resourceDescriptors, size_t threadCount) {
		using namespace DirectX;
		using namespace std;

		exception_ptr exception;

		vector<thread> threads;
		threads.reserve(threadCount);

		for (auto& pair : *this) {
			for (auto& pair1 : get<0>(pair.second)) {
				threads.push_back(thread([&] {
					try {
						auto& texture = pair1.second;

						ResourceUploadBatch resourceUploadBatch(pDevice);
						resourceUploadBatch.Begin();

						bool isCubeMap = false;

						const auto filePath = filesystem::path(DirectoryPath).append(texture.Path);
						const auto filePathString = filePath.string();
						if (!lstrcmpiW(filePath.extension().c_str(), L".dds")) {
							DX::ThrowIfFailed(CreateDDSTextureFromFile(pDevice, resourceUploadBatch, filePath.c_str(), texture.Resource.ReleaseAndGetAddressOf(), false, 0, nullptr, &isCubeMap), filePathString.c_str());

							if ((isCubeMap && pair1.first != TextureType::CubeMap) || (!isCubeMap && pair1.first == TextureType::CubeMap)) {
								throw runtime_error(filePathString + ": Invalid texture");
							}
						}
						else {
							DX::ThrowIfFailed(CreateWICTextureFromFile(pDevice, resourceUploadBatch, filePath.c_str(), texture.Resource.ReleaseAndGetAddressOf()), filePathString.c_str());
						}

						resourceUploadBatch.End(pCommandQueue).wait();

						CreateShaderResourceView(pDevice, texture.Resource.Get(), resourceDescriptors.GetCpuHandle(texture.DescriptorHeapIndex), isCubeMap);
					}
					catch (...) { if (!exception) exception = current_exception(); }
					}));

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
