module;

#include <span>
#include <string>

#include "directx/d3dx12.h"

#include "rtxmu/D3D12AccelStructManager.h"

export module RaytracingHelpers;

import CommandList;
import DeviceContext;
import ErrorHelpers;
import GPUBuffer;

using namespace ErrorHelpers;
using namespace Microsoft::WRL;
using namespace std;

export namespace DirectX::RaytracingHelpers {
	struct TopLevelAccelerationStructure {
		uint64_t ID = ~0ull;

		shared_ptr<GPUBuffer> InstanceDescs;
	};

	void BuildTopLevelAccelerationStructure(
		CommandList& commandList,
		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS flags,
		span<const D3D12_RAYTRACING_INSTANCE_DESC> descs,
		bool resize,
		TopLevelAccelerationStructure& accelerationStructure
	) {
		const auto& deviceContext = commandList.GetDeviceContext();

		const bool isValid = deviceContext.AccelerationStructureManager->IsValid(accelerationStructure.ID);

		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{
			.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL,
			.Flags = flags,
			.NumDescs = static_cast<UINT>(size(descs))
		};

		if (!resize && (flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE) && isValid) {
			inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
		}
		else {
			if (inputs.NumDescs) {
				if (resize
					|| !accelerationStructure.InstanceDescs || accelerationStructure.InstanceDescs->GetCapacity() < inputs.NumDescs) {
					accelerationStructure.InstanceDescs = GPUBuffer::CreateDefault<D3D12_RAYTRACING_INSTANCE_DESC>(deviceContext, inputs.NumDescs);
				}
			}
			else if (resize && accelerationStructure.InstanceDescs) accelerationStructure.InstanceDescs.reset();
		}

		if (inputs.NumDescs) {
			commandList.Copy(*accelerationStructure.InstanceDescs, descs);
			commandList.SetState(*accelerationStructure.InstanceDescs, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
			inputs.InstanceDescs = accelerationStructure.InstanceDescs->GetNative()->GetGPUVirtualAddress();
		}

		if (isValid) {
			if (!resize) {
				commandList.UpdateAccelerationStructures(initializer_list{ inputs }, { accelerationStructure.ID });
				return;
			}
			deviceContext.AccelerationStructureManager->RemoveAccelerationStructures({ accelerationStructure.ID });
		}
		accelerationStructure.ID = commandList.BuildAccelerationStructures(initializer_list{ inputs })[0];
	}

	D3D12_RAYTRACING_GEOMETRY_DESC CreateGeometryDesc(
		const GPUBuffer& vertices, const GPUBuffer& indices,
		D3D12_RAYTRACING_GEOMETRY_FLAGS flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE,
		D3D12_GPU_VIRTUAL_ADDRESS transform3x4 = NULL,
		DXGI_FORMAT vertexFormat = DXGI_FORMAT_R32G32B32_FLOAT
	) {
		const auto indexStride = indices.GetStride();
		if (indexStride != sizeof(uint16_t) && indexStride != sizeof(uint32_t)) {
			Throw<invalid_argument>("Triangle index format must be either uint16 or uint32");
		}
		const auto indexCount = indices.GetCapacity();
		if (indexCount % 3 != 0) Throw<invalid_argument>("Triangle index count must be divisible by 3");
		return {
			.Flags = flags,
			.Triangles{
				.Transform3x4 = transform3x4,
				.IndexFormat = indexStride == sizeof(uint16_t) ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT,
				.VertexFormat = vertexFormat,
				.IndexCount = static_cast<UINT>(indexCount),
				.VertexCount = static_cast<UINT>(vertices.GetCapacity()),
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
			vector<UINT64> Data;
		};

		ShaderBindingTable(const ShaderBindingTable&) = delete;
		ShaderBindingTable& operator=(const ShaderBindingTable&) = delete;

		ShaderBindingTable(
			CommandList& commandList, ID3D12StateObjectProperties* pStateObjectProperties,
			Entry rayGeneration, span<Entry> missEntries, span<Entry> hitGroups
		) noexcept(false) {
			constexpr auto GetStride = [](const auto& entries) {
				size_t maxCount = 0;
				for (const auto& entry : entries) maxCount = max(maxCount, size(entry.Data));
				return D3DX12Align<UINT>(D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT + sizeof(UINT64) * maxCount, D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT);
			};

			m_rayGeneration = rayGeneration;
			m_rayGenerationStride = GetStride(initializer_list{ rayGeneration });

			m_missEntries.assign_range(missEntries);
			m_missStride = GetStride(missEntries);

			m_hitGroups.assign_range(hitGroups);
			m_hitGroupStride = GetStride(hitGroups);

			vector<uint8_t> buffer(AlignRayGenerationSize() + AlignMissSize() + AlignHitGroupSize());
			auto pData = data(buffer);
			const auto Copy = [&](const auto& entries, UINT stride) {
				UINT offset = 0;
				for (const auto& [Name, Data] : entries) {
					const auto ID = pStateObjectProperties->GetShaderIdentifier(Name.c_str());
					if (ID == nullptr) {
						const auto message = L"Unknown shader identifier in Shader Binding Table: " + Name;
						const auto size = WideCharToMultiByte(CP_ACP, 0, data(message), static_cast<int>(std::size(message)), nullptr, 0, nullptr, nullptr);
						string str(size, 0);
						WideCharToMultiByte(CP_ACP, 0, data(message), static_cast<int>(std::size(message)), data(str), size, nullptr, nullptr);
						Throw<runtime_error>(str);
					}

					memcpy(memcpy(pData + offset, ID, D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT), data(Data), sizeof(UINT64) * size(Data));
					offset += stride;
				}
				pData += AlignEntrySize(offset);
			};
			Copy(initializer_list{ rayGeneration }, m_rayGenerationStride);
			Copy(missEntries, m_missStride);
			Copy(hitGroups, m_hitGroupStride);

			m_buffer = GPUBuffer::CreateDefault<uint8_t>(commandList.GetDeviceContext(), size(buffer));
			commandList.Copy(*m_buffer, buffer);
			commandList.SetUAVBarrier(*m_buffer);
		}

		D3D12_GPU_VIRTUAL_ADDRESS GetRayGenerationAddress() const noexcept { return m_buffer->GetNative()->GetGPUVirtualAddress(); }
		UINT GetRayGenerationSize() const noexcept { return m_rayGenerationStride; }

		D3D12_GPU_VIRTUAL_ADDRESS GetMissAddress() const noexcept { return GetRayGenerationAddress() + AlignRayGenerationSize(); }
		UINT GetMissStride() const noexcept { return m_missStride; }
		UINT GetMissSize() const noexcept { return m_missStride * static_cast<UINT>(size(m_missEntries)); }

		D3D12_GPU_VIRTUAL_ADDRESS GetHitGroupAddress() const noexcept { return GetMissAddress() + AlignMissSize(); }
		UINT GetHitGroupStride() const noexcept { return m_hitGroupStride; }
		UINT GetHitGroupSize() const noexcept { return m_hitGroupStride * static_cast<UINT>(size(m_hitGroups)); }

	private:
		Entry m_rayGeneration;
		vector<Entry> m_missEntries, m_hitGroups;
		UINT m_rayGenerationStride{}, m_missStride{}, m_hitGroupStride{};

		unique_ptr<GPUBuffer> m_buffer;

		static UINT AlignEntrySize(UINT size) { return D3DX12Align<UINT>(max(size, 1u), D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT); }
		UINT AlignRayGenerationSize() const noexcept { return AlignEntrySize(GetRayGenerationSize()); }
		UINT AlignMissSize() const noexcept { return AlignEntrySize(GetMissSize()); }
		UINT AlignHitGroupSize() const noexcept { return AlignEntrySize(GetHitGroupSize()); }
	};
}
