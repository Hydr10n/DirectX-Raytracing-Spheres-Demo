module;

#include "directx/d3dx12.h"

#include "directxtk12/DirectXHelpers.h"

#include "ShaderMake/ShaderBlob.h"

#include "Shaders/Raytracing.dxil.h"

export module Raytracing;

import Camera;
import CommonShaderData;
import ErrorHelpers;
import GPUBuffer;
import NRD;
import RaytracingHelpers;
import RTXGI;
import Texture;

using namespace DirectX;
using namespace DirectX::RaytracingHelpers;
using namespace ErrorHelpers;
using namespace Microsoft::WRL;
using namespace ShaderMake;
using namespace std;

#define MAKE_NAME(name) static constexpr LPCWSTR name = L#name;

export struct Raytracing {
	struct GraphicsSettings {
		XMUINT2 RenderSize;
		UINT FrameIndex, Bounces, SamplesPerPixel;
		float ThroughputThreshold = 1e-3f;
		BOOL IsRussianRouletteEnabled, IsShaderExecutionReorderingEnabled;
		struct {
			struct {
				UINT DownscaleFactor;
				float SceneScale, RoughnessThreshold;
				BOOL IsHashGridVisualizationEnabled;
			} SHARC;
		} RTXGI;
		NRDSettings NRD;
	};

	struct {
		ConstantBuffer<SceneData>* SceneData;
		ConstantBuffer<Camera>* Camera;
		UploadBuffer<InstanceData>* InstanceData;
		UploadBuffer<ObjectData>* ObjectData;
	} GPUBuffers{};

	struct {
		Texture
			* Color,
			* LinearDepth,
			* NormalizedDepth,
			* MotionVectors,
			* BaseColorMetalness,
			* EmissiveColor,
			* Normals,
			* Roughness,
			* NormalRoughness,
			* NoisyDiffuse,
			* NoisySpecular;
	} Textures{};

	explicit Raytracing(ID3D12Device5* pDevice) noexcept(false) :
		m_GPUBuffers{ .GraphicsSettings = ConstantBuffer<_GraphicsSettings>(pDevice) },
		m_RTXGI{
			.SHARC{
				.Shader = SHARC(pDevice)
			}
		} {
		const auto CreateShader = [&](Shader& shader, const ShaderConstant& constant) {
			D3D12_SHADER_BYTECODE shaderBytecode;
			FindPermutationInBlob(g_Raytracing_dxil, size(g_Raytracing_dxil), &constant, 1, &shaderBytecode.pShaderBytecode, &shaderBytecode.BytecodeLength);

			ThrowIfFailed(pDevice->CreateRootSignature(0, shaderBytecode.pShaderBytecode, shaderBytecode.BytecodeLength, IID_PPV_ARGS(&shader.RootSignature)));

			CD3DX12_STATE_OBJECT_DESC stateObjectDesc(D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE);
			const auto subobject = stateObjectDesc.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();
			subobject->SetDXILLibrary(&shaderBytecode);
			subobject->DefineExport(L"ShaderConfig");
			subobject->DefineExport(L"PipelineConfig");
			subobject->DefineExport(L"GlobalRootSignature");
			subobject->DefineExport(ShaderEntryNames::RayGeneration);
			ThrowIfFailed(pDevice->CreateStateObject(stateObjectDesc, IID_PPV_ARGS(&shader.PipelineState)));
			shader.PipelineState->SetName(L"Raytracing");

			vector<ShaderBindingTable::Entry>
				rayGenerationEntries{ { ShaderEntryNames::RayGeneration } },
				missEntries,
				hitGroupEntries;
			ComPtr<ID3D12StateObjectProperties> stateObjectProperties;
			ThrowIfFailed(shader.PipelineState->QueryInterface(IID_PPV_ARGS(&stateObjectProperties)));
			shader.BindingTable = make_unique<ShaderBindingTable>(pDevice, stateObjectProperties.Get(), rayGenerationEntries, missEntries, hitGroupEntries);
		};
		CreateShader(m_default, { "DEFAULT", "1" });
		CreateShader(m_SHARCUpdate, { "SHARC_UPDATE", "1" });
		CreateShader(m_SHARCQuery, { "SHARC_QUERY", "1" });
	}

	void SetConstants(const GraphicsSettings& graphicsSettings, const SHARCBuffers& SHARCBuffers) {
		m_GPUBuffers.GraphicsSettings.At(0) = {
			.FrameIndex = graphicsSettings.FrameIndex,
			.Bounces = graphicsSettings.Bounces,
			.SamplesPerPixel = graphicsSettings.SamplesPerPixel,
			.ThroughputThreshold = graphicsSettings.ThroughputThreshold,
			.IsRussianRouletteEnabled = graphicsSettings.IsRussianRouletteEnabled,
			.IsShaderExecutionReorderingEnabled = graphicsSettings.IsShaderExecutionReorderingEnabled,
			.RTXGI{
				.SHARC{
					.Capacity = static_cast<UINT>(SHARCBuffers.HashEntries.GetCount()),
					.SceneScale = graphicsSettings.RTXGI.SHARC.SceneScale,
					.RoughnessThreshold = graphicsSettings.RTXGI.SHARC.RoughnessThreshold,
					.IsHashGridVisualizationEnabled = graphicsSettings.RTXGI.SHARC.IsHashGridVisualizationEnabled
				}
			},
			.NRD = graphicsSettings.NRD
		};

		m_renderSize = graphicsSettings.RenderSize;

		m_RTXGI.SHARC.DownscaleFactor = graphicsSettings.RTXGI.SHARC.DownscaleFactor;
		m_RTXGI.SHARC.SceneScale = graphicsSettings.RTXGI.SHARC.SceneScale;
		m_RTXGI.SHARC.Buffers = &SHARCBuffers;
	}

	void Render(ID3D12GraphicsCommandList4* pCommandList, const TopLevelAccelerationStructure& scene, RTXGITechnique RTXGITechnique) {
		const ScopedBarrier scopedBarrier(
			pCommandList,
			{
				Textures.Color->TransitionBarrier(D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				Textures.LinearDepth->TransitionBarrier(D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				Textures.NormalizedDepth->TransitionBarrier(D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				Textures.MotionVectors->TransitionBarrier(D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				Textures.BaseColorMetalness->TransitionBarrier(D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				Textures.EmissiveColor->TransitionBarrier(D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				Textures.Normals->TransitionBarrier(D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				Textures.Roughness->TransitionBarrier(D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				Textures.NormalRoughness->TransitionBarrier(D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				Textures.NoisyDiffuse->TransitionBarrier(D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				Textures.NoisySpecular->TransitionBarrier(D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
			}
		);

		const auto DispatchRays = [&](const Shader& shader, XMUINT2 renderSize) {
			pCommandList->SetComputeRootSignature(shader.RootSignature.Get());
			pCommandList->SetPipelineState1(shader.PipelineState.Get());

			UINT i = 0;
			pCommandList->SetComputeRootShaderResourceView(i++, scene.GetBuffer()->GetGPUVirtualAddress());
			pCommandList->SetComputeRootConstantBufferView(i++, m_GPUBuffers.GraphicsSettings->GetGPUVirtualAddress());
			pCommandList->SetComputeRootConstantBufferView(i++, GPUBuffers.SceneData->GetNative()->GetGPUVirtualAddress());
			pCommandList->SetComputeRootConstantBufferView(i++, GPUBuffers.Camera->GetNative()->GetGPUVirtualAddress());
			pCommandList->SetComputeRootShaderResourceView(i++, GPUBuffers.InstanceData ? GPUBuffers.InstanceData->GetNative()->GetGPUVirtualAddress() : NULL);
			pCommandList->SetComputeRootShaderResourceView(i++, GPUBuffers.ObjectData ? GPUBuffers.ObjectData->GetNative()->GetGPUVirtualAddress() : NULL);
			pCommandList->SetComputeRootDescriptorTable(i++, Textures.Color->GetUAVDescriptor().GPUHandle);
			pCommandList->SetComputeRootDescriptorTable(i++, Textures.LinearDepth->GetUAVDescriptor().GPUHandle);
			pCommandList->SetComputeRootDescriptorTable(i++, Textures.NormalizedDepth->GetUAVDescriptor().GPUHandle);
			pCommandList->SetComputeRootDescriptorTable(i++, Textures.MotionVectors->GetUAVDescriptor().GPUHandle);
			pCommandList->SetComputeRootDescriptorTable(i++, Textures.BaseColorMetalness->GetUAVDescriptor().GPUHandle);
			pCommandList->SetComputeRootDescriptorTable(i++, Textures.EmissiveColor->GetUAVDescriptor().GPUHandle);
			pCommandList->SetComputeRootDescriptorTable(i++, Textures.Normals->GetUAVDescriptor().GPUHandle);
			pCommandList->SetComputeRootDescriptorTable(i++, Textures.Roughness->GetUAVDescriptor().GPUHandle);
			pCommandList->SetComputeRootDescriptorTable(i++, Textures.NormalRoughness->GetUAVDescriptor().GPUHandle);
			pCommandList->SetComputeRootDescriptorTable(i++, Textures.NoisyDiffuse->GetUAVDescriptor().GPUHandle);
			pCommandList->SetComputeRootDescriptorTable(i++, Textures.NoisySpecular->GetUAVDescriptor().GPUHandle);

			if (RTXGITechnique == RTXGITechnique::SHARC) {
				const auto& buffers = *m_RTXGI.SHARC.Buffers;
				pCommandList->SetComputeRootUnorderedAccessView(i++, buffers.HashEntries->GetGPUVirtualAddress());
				pCommandList->SetComputeRootUnorderedAccessView(i++, buffers.PreviousVoxelData->GetGPUVirtualAddress());
				pCommandList->SetComputeRootUnorderedAccessView(i++, buffers.VoxelData->GetGPUVirtualAddress());
			}

			const D3D12_DISPATCH_RAYS_DESC dispatchRaysDesc{
				.RayGenerationShaderRecord{
					.StartAddress = shader.BindingTable->GetRayGenerationAddress(),
					.SizeInBytes = shader.BindingTable->GetRayGenerationSize()
				},
				.MissShaderTable{
					.StartAddress = shader.BindingTable->GetMissAddress(),
					.SizeInBytes = shader.BindingTable->GetMissSize(),
					.StrideInBytes = shader.BindingTable->GetMissStride()
				},
				.HitGroupTable{
					.StartAddress = shader.BindingTable->GetHitGroupAddress(),
					.SizeInBytes = shader.BindingTable->GetHitGroupSize(),
					.StrideInBytes = shader.BindingTable->GetHitGroupStride()
				},
				.Width = renderSize.x,
				.Height = renderSize.y,
				.Depth = 1
			};
			pCommandList->DispatchRays(&dispatchRaysDesc);
		};
		switch (RTXGITechnique) {
			case RTXGITechnique::None: DispatchRays(m_default, m_renderSize); break;

			case RTXGITechnique::SHARC:
			{
				DispatchRays(m_SHARCUpdate, { m_renderSize.x / m_RTXGI.SHARC.DownscaleFactor, m_renderSize.y / m_RTXGI.SHARC.DownscaleFactor });

				m_RTXGI.SHARC.Shader.GPUBuffers.Camera = GPUBuffers.Camera;

				m_RTXGI.SHARC.Shader.Process(pCommandList, *m_RTXGI.SHARC.Buffers, { .SceneScale = m_RTXGI.SHARC.SceneScale });

				{
					const auto barriers = {
						CD3DX12_RESOURCE_BARRIER::UAV(m_RTXGI.SHARC.Buffers->HashEntries),
						CD3DX12_RESOURCE_BARRIER::UAV(m_RTXGI.SHARC.Buffers->VoxelData)
					};
					pCommandList->ResourceBarrier(static_cast<UINT>(size(barriers)), data(barriers));
				}

				DispatchRays(m_SHARCQuery, m_renderSize);
			}
			break;
		}
	}

private:
	struct _GraphicsSettings {
		UINT FrameIndex, Bounces, SamplesPerPixel;
		float ThroughputThreshold;
		BOOL IsRussianRouletteEnabled, IsShaderExecutionReorderingEnabled;
		XMUINT2 _;
		struct {
			struct {
				UINT Capacity;
				float SceneScale, RoughnessThreshold;
				BOOL IsHashGridVisualizationEnabled;
			} SHARC;
		} RTXGI;
		NRDSettings NRD;
	};

	struct { ConstantBuffer<_GraphicsSettings> GraphicsSettings; } m_GPUBuffers;

	struct ShaderEntryNames { MAKE_NAME(RayGeneration); };

	struct Shader {
		ComPtr<ID3D12RootSignature> RootSignature;
		ComPtr<ID3D12StateObject> PipelineState;

		unique_ptr<ShaderBindingTable> BindingTable;
	} m_default, m_SHARCUpdate, m_SHARCQuery;

	XMUINT2 m_renderSize{};

	struct {
		struct {
			UINT DownscaleFactor{};
			float SceneScale{};

			const SHARCBuffers* Buffers{};

			SHARC Shader;
		} SHARC;
	} m_RTXGI;
};
