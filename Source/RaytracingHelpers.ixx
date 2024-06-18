module;

#include <memory>
#include <span>
#include <stdexcept>

#include "directxtk12/DirectXHelpers.h"

export module RaytracingHelpers;

import ErrorHelpers;
import GPUBuffer;

using namespace ErrorHelpers;
using namespace Microsoft::WRL;
using namespace std;

export namespace DirectX::RaytracingHelpers {
	struct BottomLevelAccelerationStructureBuffers {
		shared_ptr<DefaultBuffer<uint8_t>> Scratch, Result;

		BottomLevelAccelerationStructureBuffers() = default;

		BottomLevelAccelerationStructureBuffers(ID3D12Device* pDevice, size_t scratchSize, size_t resultSize) noexcept(false) {
			Scratch = make_shared<DefaultBuffer<uint8_t>>(pDevice, scratchSize);
			Result = make_shared<DefaultBuffer<uint8_t>>(pDevice, resultSize, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
		}
	};

	struct TopLevelAccelerationStructureBuffers : BottomLevelAccelerationStructureBuffers {
		shared_ptr<UploadBuffer<D3D12_RAYTRACING_INSTANCE_DESC>> InstanceDescs;

		TopLevelAccelerationStructureBuffers() = default;

		TopLevelAccelerationStructureBuffers(ID3D12Device* pDevice, size_t scratchSize, size_t resultSize, size_t instanceDescCount) :
			BottomLevelAccelerationStructureBuffers(pDevice, scratchSize, resultSize) {
			if (instanceDescCount) InstanceDescs = make_shared<UploadBuffer<D3D12_RAYTRACING_INSTANCE_DESC>>(pDevice, instanceDescCount);
		}
	};

	template <bool IsTop>
	class AccelerationStructure {
	public:
		AccelerationStructure(const AccelerationStructure&) = delete;
		AccelerationStructure& operator=(const AccelerationStructure&) = delete;

		AccelerationStructure(ID3D12Device5* pDevice, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE) : m_device(pDevice), m_flags(flags) {}

		ID3D12Resource* GetBuffer() const noexcept { return m_buffers.Result->GetNative(); }

		UINT GetDescCount() const noexcept { return m_descCount; }

		void Build(ID3D12GraphicsCommandList4* pCommandList, span<const conditional_t<IsTop, D3D12_RAYTRACING_INSTANCE_DESC, D3D12_RAYTRACING_GEOMETRY_DESC>> descs, bool updateOnly) {
			m_descCount = static_cast<UINT>(size(descs));

			D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc;
			desc.Inputs = {
				.Flags = m_flags,
				.NumDescs = m_descCount
			};
			if constexpr (IsTop) desc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
			else {
				desc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
				desc.Inputs.pGeometryDescs = data(descs);
			}

			if (updateOnly && m_flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE && m_buffers.Result) {
				desc.Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
				desc.SourceAccelerationStructureData = m_buffers.Result->GetNative()->GetGPUVirtualAddress();
			}
			else {
				D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
				m_device->GetRaytracingAccelerationStructurePrebuildInfo(&desc.Inputs, &info);

				if constexpr (IsTop) {
					m_buffers = TopLevelAccelerationStructureBuffers(m_device, info.ScratchDataSizeInBytes, info.ResultDataMaxSizeInBytes, desc.Inputs.NumDescs);
				}
				else {
					m_buffers = BottomLevelAccelerationStructureBuffers(m_device, info.ScratchDataSizeInBytes, info.ResultDataMaxSizeInBytes);
				}

				desc.SourceAccelerationStructureData = NULL;
			}

			if constexpr (IsTop) {
				if (m_buffers.InstanceDescs) {
					m_buffers.InstanceDescs->Upload(descs);

					desc.Inputs.InstanceDescs = m_buffers.InstanceDescs->GetNative()->GetGPUVirtualAddress();
				}
			}

			desc.DestAccelerationStructureData = m_buffers.Result->GetNative()->GetGPUVirtualAddress();
			desc.ScratchAccelerationStructureData = m_buffers.Scratch->GetNative()->GetGPUVirtualAddress();
			pCommandList->BuildRaytracingAccelerationStructure(&desc, 0, nullptr);

			m_buffers.Result->InsertUAVBarrier(pCommandList);
		}

	private:
		ID3D12Device5* m_device;

		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS m_flags;

		UINT m_descCount{};

		conditional_t<IsTop, TopLevelAccelerationStructureBuffers, BottomLevelAccelerationStructureBuffers> m_buffers;
	};

	using BottomLevelAccelerationStructure = AccelerationStructure<false>;
	using TopLevelAccelerationStructure = AccelerationStructure<true>;

	template <typename Vertex, D3D12_HEAP_TYPE VertexHeapType, size_t VertexAlignment, typename Index, D3D12_HEAP_TYPE IndexHeapType> requires same_as<Index, UINT16> || same_as<Index, UINT32>
	D3D12_RAYTRACING_GEOMETRY_DESC CreateGeometryDesc(
		const TGPUBuffer<Vertex, VertexHeapType, VertexAlignment>&vertices, const TGPUBuffer<Index, IndexHeapType>&indices,
		D3D12_RAYTRACING_GEOMETRY_FLAGS flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE,
		D3D12_GPU_VIRTUAL_ADDRESS transform3x4 = NULL,
		DXGI_FORMAT vertexFormat = DXGI_FORMAT_R32G32B32_FLOAT
	) {
		const auto vertexCount = vertices.GetCount(), indexCount = indices.GetCount();
		if (indexCount % 3 != 0) Throw<invalid_argument>("Triangle index count must be divisible by 3");
		return {
			.Flags = flags,
			.Triangles{
				.Transform3x4 = transform3x4,
				.IndexFormat = is_same_v<Index, UINT16> ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT,
				.VertexFormat = vertexFormat,
				.IndexCount = static_cast<UINT>(indexCount),
				.VertexCount = static_cast<UINT>(vertexCount),
				.IndexBuffer = indices->GetGPUVirtualAddress(),
				.VertexBuffer{
					.StartAddress = vertices->GetGPUVirtualAddress(),
					.StrideInBytes = vertices.GetStride()
				}
			}
		};
	}

	class ShaderBindingTable {
	public:
		struct Entry {
			wstring Name;
			vector<UINT64> Data{ 0 };
		};

		ShaderBindingTable(const ShaderBindingTable&) = delete;
		ShaderBindingTable& operator=(const ShaderBindingTable&) = delete;

		ShaderBindingTable(
			ID3D12Device* pDevice, ID3D12StateObjectProperties* pStateObjectProperties,
			span<Entry> rayGenerationEntries, span<Entry> missEntries, span<Entry> hitGroupEntries
		) {
			constexpr auto GetStride = [](span<Entry> entries) {
				size_t maxCount = 0;
				for (const auto& entry : entries) maxCount = max(maxCount, size(entry.Data));
				return static_cast<UINT>(AlignUp(D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT + sizeof(UINT64) * maxCount, D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT));
			};

			m_rayGenerationEntries.assign_range(rayGenerationEntries);
			m_rayGenerationStride = GetStride(rayGenerationEntries);

			m_missEntries.assign_range(missEntries);
			m_missStride = GetStride(missEntries);

			m_hitGroupEntries.assign_range(hitGroupEntries);
			m_hitGroupStride = GetStride(hitGroupEntries);

			m_buffer = make_unique<ConstantBuffer<uint8_t>>(pDevice, (GetRayGenerationSize() + GetMissSize() + GetHitGroupSize() + D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1) / D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

			auto pData = &m_buffer->At(0);
			const auto Copy = [&](span<const Entry> entries, UINT stride) {
				for (const auto& [Name, Data] : entries) {
					const auto ID = pStateObjectProperties->GetShaderIdentifier(Name.c_str());
					if (ID == nullptr) {
						const auto message = L"Unknown shader identifier in Shader Binding Table: " + Name;
						const auto size = WideCharToMultiByte(CP_ACP, 0, data(message), static_cast<int>(std::size(message)), nullptr, 0, nullptr, nullptr);
						string str(size, 0);
						WideCharToMultiByte(CP_ACP, 0, data(message), static_cast<int>(std::size(message)), data(str), size, nullptr, nullptr);
						throw logic_error(str);
					}

					memcpy(pData, ID, D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT);
					memcpy(pData + D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT, data(Data), sizeof(UINT64) * size(Data));

					pData += stride;
				}
			};
			Copy(m_rayGenerationEntries, m_rayGenerationStride);
			Copy(m_missEntries, m_missStride);
			Copy(m_hitGroupEntries, m_hitGroupStride);
		}

		D3D12_GPU_VIRTUAL_ADDRESS GetRayGenerationAddress() const { return m_buffer->GetNative()->GetGPUVirtualAddress(); }
		UINT GetRayGenerationStride() const { return m_rayGenerationStride; }
		UINT GetRayGenerationSize() const { return m_rayGenerationStride * static_cast<UINT>(size(m_rayGenerationEntries)); }

		D3D12_GPU_VIRTUAL_ADDRESS GetMissAddress() const { return GetRayGenerationAddress() + GetMissSize(); }
		UINT GetMissStride() const { return m_missStride; }
		UINT GetMissSize() const { return m_missStride * static_cast<UINT>(size(m_missEntries)); }

		D3D12_GPU_VIRTUAL_ADDRESS GetHitGroupAddress() const { return GetMissAddress() + GetHitGroupSize(); }
		UINT GetHitGroupStride() const { return m_hitGroupStride; }
		UINT GetHitGroupSize() const { return m_hitGroupStride * static_cast<UINT>(size(m_hitGroupEntries)); }

	private:
		vector<Entry> m_rayGenerationEntries, m_missEntries, m_hitGroupEntries;
		UINT m_rayGenerationStride{}, m_missStride{}, m_hitGroupStride{};

		unique_ptr<ConstantBuffer<uint8_t>> m_buffer;
	};
}
