#pragma once

#pragma warning(push)
#pragma warning(disable: 26812)

#include "pch.h"

#include "DeviceResources.h"
#include "GraphicsMemory.h"
#include "ResourceUploadBatch.h"
#include "DescriptorHeap.h"

#include "DirectXHelpers.h"

#include "StepTimer.h"

#include "GamePad.h"
#include "Keyboard.h"
#include "Mouse.h"

#include "OrbitCamera.h"

#include "GeometricPrimitive.h"

#include "WICTextureLoader.h"

#include "SimpleMath.h"
#include "Random.h"

#include "BottomLevelASGenerator.h"
#include "TopLevelASGenerator.h"
#include "RootSignatureGenerator.h"
#include "RaytracingPipelineGenerator.h"
#include "ShaderBindingTableGenerator.h"

#include "RaytracingGeometryHelpers.h"

#include "TransformCollection.h"

#include "Materials.h"

#include "Shaders/Raytracing.hlsl.h"

#include <map>

#include <filesystem>

#pragma warning(pop)

class D3DApp : public DX::IDeviceNotify {
public:
	static constexpr UINT MaxAntiAliasingSampleCount = 8;

	D3DApp(const D3DApp&) = delete;
	D3DApp& operator=(const D3DApp&) = delete;

	D3DApp(HWND hWnd, const SIZE& outputSize) noexcept(false) {
		srand(static_cast<UINT>(GetTickCount64()));

		m_deviceResources->RegisterDeviceNotify(this);

		m_deviceResources->SetWindow(hWnd, static_cast<int>(outputSize.cx), static_cast<int>(outputSize.cy));

		m_deviceResources->CreateDeviceResources();
		CreateDeviceDependentResources();

		m_deviceResources->CreateWindowSizeDependentResources();
		CreateWindowSizeDependentResources();

		m_mouse->SetWindow(hWnd);

		m_orbitCamera.SetRadius(m_cameraRadius, MinCameraRadius, MaxCameraRadius);
	}

	UINT GetAntiAliasingSampleCount() const { return m_antiAliasingSampleCount; }

	bool SetAntiAliasingSampleCount(UINT count) {
		if (count < 1 || count > MaxAntiAliasingSampleCount) return false;
		m_antiAliasingSampleCount = count;
		return true;
	}

	SIZE GetOutputSize() const {
		const auto rc = m_deviceResources->GetOutputSize();
		return { rc.right - rc.left, rc.bottom - rc.top };
	}

	void Tick() {
		m_stepTimer.Tick([&] { Update(); });

		Render();
	}

	void OnWindowSizeChanged(const SIZE& size) {
		RECT rc;
		DX::ThrowIfFailed(GetClientRect(m_deviceResources->GetWindow(), &rc));
		m_orbitCamera.SetWindow(static_cast<int>(rc.right - rc.left), static_cast<int>(rc.bottom - rc.top));

		if (!m_deviceResources->WindowSizeChanged(static_cast<int>(size.cx), static_cast<int>(size.cy))) return;

		CreateWindowSizeDependentResources();
	}

	void OnActivated() {}

	void OnDeactivated() {}

	void OnResuming() {
		m_stepTimer.ResetElapsedTime();

		m_gamepad->Resume();
	}

	void OnSuspending() { m_gamepad->Suspend(); }

	void OnDeviceLost() override {
		m_output.Reset();

		m_shaderBindingTable.Reset();

		m_materialConstantBuffer.Reset();
		m_objectConstantBuffer.Reset();
		m_sceneConstantBuffer.Reset();

		m_topLevelASBuffers = {};
		m_bottomLevelASBuffers = {};

		m_sphere.reset();

		m_renderItems.clear();

		m_textures.clear();

		m_resourceDescriptors.reset();

		m_pipelineStateObject.Reset();

		m_primaryRayClosestHitSignature.Reset();
		m_rayGenerationSignature.Reset();

		m_graphicsMemory.reset();
	}

	void OnDeviceRestored() override {
		CreateDeviceDependentResources();

		CreateWindowSizeDependentResources();
	}

private:
	enum class DescriptorHeapIndex { Output, Scene, SphereVertices, SphereIndices, EarthTexture, MoonTexture, Count };

	struct Texture {
		size_t SrvHeapIndex = SIZE_MAX;
		Microsoft::WRL::ComPtr<ID3D12Resource> Resource;
	};

	struct RenderItem {
		std::string Name;
		size_t VerticesSrvHeapIndex = SIZE_MAX, ObjectConstantBufferIndex = SIZE_MAX;
		DirectX::XMFLOAT3 Position{};
		TransformCollection Transforms;
		std::shared_ptr<Material> Material;
		std::shared_ptr<Texture> Texture;
	};

	struct alignas(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT) SceneConstant {
		DirectX::XMFLOAT4X4 ProjectionToWorld;
		DirectX::XMFLOAT3 CameraPosition;
		UINT MaxTraceRecursionDepth;
		UINT AntiAliasingSampleCount;
		UINT FrameCount;
	};

	struct alignas(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT) ObjectConstant { BOOL IsTextureUsed; };

	struct alignas(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT) MaterialConstant : MaterialBase {};

	static constexpr UINT MaxTraceRecursionDepth = 10;

	static constexpr float MinCameraRadius = 1, MaxCameraRadius = 25;

	static constexpr LPCWSTR RayGeneration = L"RayGeneration",
		PrimaryRayMiss = L"PrimaryRayMiss", PrimaryRayClosestHit = L"PrimaryRayClosestHit",
		SphereHitGroup = L"SphereHitGroup";

	static constexpr LPCSTR Earth = "Earth", Moon = "Moon", Wave = "Wave";

	const std::unique_ptr<DirectX::GamePad> m_gamepad = std::make_unique<decltype(m_gamepad)::element_type>();
	const std::unique_ptr<DirectX::Keyboard> m_keyboard = std::make_unique<decltype(m_keyboard)::element_type>();
	const std::unique_ptr<DirectX::Mouse> m_mouse = std::make_unique<decltype(m_mouse)::element_type>();

	std::unique_ptr<DX::DeviceResources> m_deviceResources = std::make_unique<decltype(m_deviceResources)::element_type>();
	std::unique_ptr<DirectX::GraphicsMemory> m_graphicsMemory;

	DX::StepTimer m_stepTimer;

	DirectX::GamePad::ButtonStateTracker m_gamepadButtonStateTrackers[DirectX::GamePad::MAX_PLAYER_COUNT];
	DirectX::Keyboard::KeyboardStateTracker m_keyboardStateTracker;
	DirectX::Mouse::ButtonStateTracker m_mouseButtonStateTracker;

	float m_cameraRadius = 14;
	DX::OrbitCamera m_orbitCamera;

	UINT m_antiAliasingSampleCount{};

	nv_helpers_dx12::ShaderBindingTableGenerator m_shaderBindingTableGenerator;

	Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rayGenerationSignature, m_primaryRayClosestHitSignature;

	Microsoft::WRL::ComPtr<ID3D12StateObject> m_pipelineStateObject;

	std::unique_ptr<DirectX::DescriptorHeap> m_resourceDescriptors;

	std::map<std::string, std::shared_ptr<Texture>> m_textures;
	std::vector<RenderItem> m_renderItems;

	std::shared_ptr<RaytracingHelpers::Triangles<DirectX::VertexPositionNormalTexture, UINT16>> m_sphere;

	RaytracingHelpers::AccelerationStructureBuffers m_bottomLevelASBuffers, m_topLevelASBuffers;

	DirectX::GraphicsResource m_sceneConstantBuffer, m_objectConstantBuffer, m_materialConstantBuffer;

	Microsoft::WRL::ComPtr<ID3D12Resource> m_shaderBindingTable;

	Microsoft::WRL::ComPtr<ID3D12Resource> m_output;

	static void CopyOutputToRenderTarget(ID3D12GraphicsCommandList* pCommandList, ID3D12Resource* pOutput, ID3D12Resource* pRenderTarget) {
		using namespace DirectX;

		TransitionResource(pCommandList, pRenderTarget, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
		TransitionResource(pCommandList, pOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);

		pCommandList->CopyResource(pRenderTarget, pOutput);

		TransitionResource(pCommandList, pOutput, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		TransitionResource(pCommandList, pRenderTarget, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
	}

	static auto CreateSphere(ID3D12Device* pDevice, float diameter = 1, size_t tessellation = 3) {
		DirectX::GeometricPrimitive::VertexCollection vertices;
		DirectX::GeometricPrimitive::IndexCollection indices;
		DirectX::GeometricPrimitive::CreateGeoSphere(vertices, indices, diameter, tessellation, false);
		return RaytracingHelpers::Triangles(pDevice, vertices, indices, D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE);
	}

	static TransformCollection CreateTransformsForAstronomicalObject(float orbitRadius = 0, float rotationY = 0, float revolutionY = 0, const DirectX::XMFLOAT3& scaling = { 1, 1, 1 }) { return TransformCollection{ Transform::CreateScaling(scaling), Transform::CreateRotationY(rotationY), Transform::CreateTranslation({ orbitRadius * cos(revolutionY), 0, orbitRadius * sin(revolutionY)}) }; }

	void Clear() {
		const auto commandList = m_deviceResources->GetCommandList();

		PIXBeginEvent(commandList, PIX_COLOR_DEFAULT, L"Clear");

		const auto rtvDescriptor = m_deviceResources->GetRenderTargetView(), dsvDescriptor = m_deviceResources->GetDepthStencilView();
		commandList->OMSetRenderTargets(1, &rtvDescriptor, FALSE, &dsvDescriptor);
		commandList->ClearRenderTargetView(rtvDescriptor, DirectX::Colors::CornflowerBlue, 0, nullptr);
		commandList->ClearDepthStencilView(dsvDescriptor, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1, 0, 0, nullptr);

		const auto viewport = m_deviceResources->GetScreenViewport();
		const auto scissorRect = m_deviceResources->GetScissorRect();
		commandList->RSSetViewports(1, &viewport);
		commandList->RSSetScissorRects(1, &scissorRect);

		PIXEndEvent(commandList);
	}

	void Render() {
		if (!m_stepTimer.GetFrameCount()) return;

		m_deviceResources->Prepare();

		Clear();

		const auto commandList = m_deviceResources->GetCommandList();

		PIXBeginEvent(commandList, PIX_COLOR_DEFAULT, L"Render");

		TraceRays();

		CopyOutputToRenderTarget(commandList, m_output.Get(), m_deviceResources->GetRenderTarget());

		PIXEndEvent(commandList);

		PIXBeginEvent(PIX_COLOR_DEFAULT, L"Present");

		m_deviceResources->Present(D3D12_RESOURCE_STATE_PRESENT);

		m_graphicsMemory->Commit(m_deviceResources->GetCommandQueue());

		PIXEndEvent();
	}

	void Update() {
		using namespace DirectX;

		PIXBeginEvent(PIX_COLOR_DEFAULT, L"Update");

		GamePad::State gamepadStates[GamePad::MAX_PLAYER_COUNT];
		for (int i = 0; i < GamePad::MAX_PLAYER_COUNT; i++) {
			gamepadStates[i] = m_gamepad->GetState(i);
			m_gamepadButtonStateTrackers[i].Update(gamepadStates[i]);
		}

		const auto keyboardState = m_keyboard->GetState();
		m_keyboardStateTracker.Update(keyboardState);

		const auto mouseState = m_mouse->GetState(), lastMouseState = m_mouseButtonStateTracker.GetLastState();
		m_mouseButtonStateTracker.Update(mouseState);

		UpdateCamera(gamepadStates, mouseState, lastMouseState);

		UpdateSceneConstantBuffer();

		UpdateRenderItems();

		PIXEndEvent();
	}

	void CreateDeviceDependentResources() {
		const auto device = m_deviceResources->GetD3DDevice();

		m_graphicsMemory = std::make_unique<decltype(m_graphicsMemory)::element_type>(device);

		LoadTextures();

		CreateRootSignatures();

		CreatePipelineStateObjects();

		CreateDescriptorHeaps();

		CreateRenderItems();

		CreateAccelerationStructures();

		CreateConstantBuffers();

		CreateShaderBindingTables();

		CreateWindowSizeIndependentShaderResourceViews();
	}

	void CreateWindowSizeDependentResources() { CreateWindowSizeDependentShaderResourceViews(); }

	void LoadTextures() {
		using namespace DirectX;

		const auto device = m_deviceResources->GetD3DDevice();

		ResourceUploadBatch resourceUpload(device);
		resourceUpload.Begin();

		const auto path = std::filesystem::path(*__wargv).replace_filename(L"Textures\\").wstring();

		constexpr struct {
			LPCSTR Name;
			LPCWSTR Path;
			DescriptorHeapIndex DescriptorHeapIndex;
		} TextureInfos[]{
			{ Earth, L"Earth.png", DescriptorHeapIndex::EarthTexture },
			{ Moon, L"Moon.png", DescriptorHeapIndex::MoonTexture }
		};
		for (const auto& textureInfo : TextureInfos) {
			const auto texture = std::make_shared<Texture>();
			texture->SrvHeapIndex = static_cast<size_t>(textureInfo.DescriptorHeapIndex);
			DX::ThrowIfFailed(CreateWICTextureFromFile(device, resourceUpload, (path + textureInfo.Path).c_str(), texture->Resource.ReleaseAndGetAddressOf()));
			m_textures[textureInfo.Name] = texture;
		}

		resourceUpload.End(m_deviceResources->GetCommandQueue()).wait();
	}

	void CreateRootSignatures() {
		using namespace nv_helpers_dx12;

		const auto device = m_deviceResources->GetD3DDevice();

		{
			RootSignatureGenerator rootSignatureGenerator;

			CD3DX12_ROOT_PARAMETER rootParameter;

			const D3D12_DESCRIPTOR_RANGE descriptorRanges[]{
				CD3DX12_DESCRIPTOR_RANGE(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0),
				CD3DX12_DESCRIPTOR_RANGE(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 1)
			};
			rootParameter.InitAsDescriptorTable(ARRAYSIZE(descriptorRanges), descriptorRanges);
			rootSignatureGenerator.AddRootParameter(rootParameter);

			rootParameter.InitAsConstantBufferView(0);
			rootSignatureGenerator.AddRootParameter(rootParameter);

			m_rayGenerationSignature.Attach(rootSignatureGenerator.Generate(device, D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE));
		}

		{
			RootSignatureGenerator rootSignatureGenerator;

			CD3DX12_ROOT_PARAMETER rootParameter;

			const D3D12_DESCRIPTOR_RANGE descriptorRanges[]{
				CD3DX12_DESCRIPTOR_RANGE(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0)
			};
			rootParameter.InitAsDescriptorTable(ARRAYSIZE(descriptorRanges), descriptorRanges);
			rootSignatureGenerator.AddRootParameter(rootParameter);

			const D3D12_DESCRIPTOR_RANGE descriptorRanges1[]{
				CD3DX12_DESCRIPTOR_RANGE(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, 0),
				CD3DX12_DESCRIPTOR_RANGE(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2, 0, 1)
			};
			rootParameter.InitAsDescriptorTable(ARRAYSIZE(descriptorRanges1), descriptorRanges1);
			rootSignatureGenerator.AddRootParameter(rootParameter);

			const D3D12_DESCRIPTOR_RANGE descriptorRanges2[]{
				CD3DX12_DESCRIPTOR_RANGE(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3)
			};
			rootParameter.InitAsDescriptorTable(ARRAYSIZE(descriptorRanges2), descriptorRanges2);
			rootSignatureGenerator.AddRootParameter(rootParameter);

			for (UINT i = 0; i < 3; i++) {
				rootParameter.InitAsConstantBufferView(i);
				rootSignatureGenerator.AddRootParameter(rootParameter);
			}

			m_primaryRayClosestHitSignature.Attach(rootSignatureGenerator.Generate(device, D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE));
		}
	}

	void CreatePipelineStateObjects() {
		nv_helpers_dx12::RaytracingPipelineGenerator raytracingPipelineGenerator(m_deviceResources->GetD3DDevice());
		raytracingPipelineGenerator.AddLibrary({ g_pRaytracing, sizeof(g_pRaytracing) }, { RayGeneration, PrimaryRayMiss, PrimaryRayClosestHit });

		raytracingPipelineGenerator.AddRootSignatureAssociation(m_rayGenerationSignature.Get(), { RayGeneration });
		raytracingPipelineGenerator.AddRootSignatureAssociation(m_primaryRayClosestHitSignature.Get(), { PrimaryRayClosestHit });

		raytracingPipelineGenerator.AddHitGroup(SphereHitGroup, PrimaryRayClosestHit);

		raytracingPipelineGenerator.SetMaxPayloadSize(sizeof(DirectX::XMFLOAT4) * 2);

		raytracingPipelineGenerator.SetMaxTraceRecursionDepth(MaxTraceRecursionDepth);

		m_pipelineStateObject.Attach(raytracingPipelineGenerator.Generate());
	}

	void CreateDescriptorHeaps() { m_resourceDescriptors = std::make_unique<DirectX::DescriptorHeap>(m_deviceResources->GetD3DDevice(), static_cast<size_t>(DescriptorHeapIndex::Count)); }

	void CreateRenderItems() {
		using namespace DirectX;

		m_renderItems.clear();

		m_renderItems.push_back(RenderItem{
			.VerticesSrvHeapIndex = static_cast<size_t>(DescriptorHeapIndex::SphereVertices),
			.ObjectConstantBufferIndex = 0,
			.Position = { 0, -0.5e2f - 0.1f, 0 },
			.Transforms = { Transform::CreateScaling({ 1e2f, 1e2f, 1e2f }) },
			.Material = std::make_shared<Material>(MaterialBase::CreateMetal({ 0.5f, 0.5f, 0.5f, 1 }, 0), 0)
			});

		const struct {
			XMFLOAT3 Position;
			std::shared_ptr<Material> Material;
			std::shared_ptr<Texture> Texture;
		} renderItems[]{
			{
				.Position = { -2, 0.5f, 0 },
				.Material = std::make_shared<Material>(MaterialBase::CreateLambertian({ 0.1f, 0.2f, 0.5f, 1 }), 1)
			},
			{
				.Position = { 0, 0.5f, 0 },
				.Material = std::make_shared<Material>(MaterialBase::CreateDielectric({ 1, 1, 1, 1 }, 1.5f), 2)
			},
			{
				.Position = { 2, 0.5f, 0 },
				.Material = std::make_shared<Material>(MaterialBase::CreateMetal({ 0.7f, 0.6f, 0.5f, 1 }, 0.2f), 3)
			}
		};
		for (const auto& renderItem : renderItems) {
			m_renderItems.push_back(RenderItem{
				.VerticesSrvHeapIndex = static_cast<size_t>(DescriptorHeapIndex::SphereVertices),
				.ObjectConstantBufferIndex = m_renderItems.size(),
				.Position = renderItem.Position,
				.Material = renderItem.Material,
				.Texture = renderItem.Texture
				});
		}

		for (int a = -10; a < 10; a++) {
			for (int b = -10; b < 10; b++) {
				RenderItem renderItem;

				renderItem.Name = Wave;

				renderItem.Transforms = { Transform::CreateScaling({0.15f, 0.15f, 0.15f}), Transform::CreateTranslation() };

				const XMFLOAT3 position(a + 0.7f * Random::Float(), 0.1f, b - 0.7f * Random::Float());

				bool collision = false;
				for (const auto& renderItem : renderItems) {
					if ((SimpleMath::Vector3(position) - SimpleMath::Vector3(renderItem.Position)).Length() <= (1 + 0.15f) / 2) {
						collision = true;
						break;
					}
				}
				if (collision) continue;

				renderItem.VerticesSrvHeapIndex = static_cast<size_t>(DescriptorHeapIndex::SphereVertices);

				const auto constantBufferIndex = m_renderItems.size();

				renderItem.ObjectConstantBufferIndex = constantBufferIndex;

				renderItem.Position = position;

				const auto randomValue = Random::Float();
				if (randomValue < 0.5f) {
					renderItem.Material = std::make_shared<Material>(MaterialBase::CreateLambertian(Random::Float4()), constantBufferIndex);
				}
				else if (randomValue < 0.75f) {
					renderItem.Material = std::make_shared<Material>(MaterialBase::CreateMetal(Random::Float4(0.5f, 1), Random::Float(0, 0.5f)), constantBufferIndex);
				}
				else {
					renderItem.Material = std::make_shared<Material>(MaterialBase::CreateDielectric(Random::Float4(), 1.5f), constantBufferIndex);
				}

				m_renderItems.emplace_back(renderItem);
			}
		}

		constexpr LPCSTR AstronomicalObjects[]{ Earth, Moon };
		for (const auto astronomicalObject : AstronomicalObjects) {
			m_renderItems.push_back(RenderItem{
				.Name = astronomicalObject,
				.VerticesSrvHeapIndex = static_cast<size_t>(DescriptorHeapIndex::SphereVertices),
				.ObjectConstantBufferIndex = m_renderItems.size(),
				.Position = { 0, 4, 0 },
				.Material = std::make_shared<Material>(MaterialBase::CreateLambertian({ 0.5, 0.5, 0.5, 1 }), m_renderItems.size()),
				.Texture = m_textures[astronomicalObject]
				});
		}
	}

	void CreateBottomLevelAS(const std::vector<D3D12_RAYTRACING_GEOMETRY_DESC>& geometryDescs, RaytracingHelpers::AccelerationStructureBuffers& buffers) {
		const auto device = m_deviceResources->GetD3DDevice();

		nv_helpers_dx12::BottomLevelASGenerator bottomLevelASGenerator;

		for (const auto& geometryDesc : geometryDescs) bottomLevelASGenerator.AddGeometry(geometryDesc);

		UINT64 scratchSize, resultSize;
		bottomLevelASGenerator.ComputeASBufferSizes(device, false, &scratchSize, &resultSize);

		buffers = RaytracingHelpers::AccelerationStructureBuffers(device, scratchSize, resultSize);

		bottomLevelASGenerator.Generate(m_deviceResources->GetCommandList(), buffers.Scratch.Get(), buffers.Result.Get());
	}

	void CreateTopLevelAS(bool update, RaytracingHelpers::AccelerationStructureBuffers& buffers) {
		const auto device = m_deviceResources->GetD3DDevice();

		const auto commandList = m_deviceResources->GetCommandList();

		nv_helpers_dx12::TopLevelASGenerator topLevelASGenerator;

		for (UINT size = static_cast<UINT>(m_renderItems.size()), i = 0; i < size; i++) topLevelASGenerator.AddInstance(m_bottomLevelASBuffers.Result->GetGPUVirtualAddress(), m_renderItems[i].Transforms.Transform() * DirectX::XMMatrixTranslationFromVector(DirectX::XMLoadFloat3(&m_renderItems[i].Position)), i, i);

		if (update) {
			const auto uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(buffers.Result.Get());
			commandList->ResourceBarrier(1, &uavBarrier);
		}
		else {
			UINT64 scratchSize, resultSize, instanceDescsSize;
			topLevelASGenerator.ComputeASBufferSizes(device, update, &scratchSize, &resultSize, &instanceDescsSize);
			buffers = RaytracingHelpers::AccelerationStructureBuffers(device, scratchSize, resultSize, instanceDescsSize);
		}

		topLevelASGenerator.Generate(commandList, buffers.Scratch.Get(), buffers.Result.Get(), buffers.InstanceDesc.Get());
	}

	void CreateAccelerationStructures() {
		const auto device = m_deviceResources->GetD3DDevice();

		const auto commandList = m_deviceResources->GetCommandList();

		commandList->Reset(m_deviceResources->GetCommandAllocator(), nullptr);

		m_sphere = std::make_shared<decltype(m_sphere)::element_type>(CreateSphere(device, 1, 6));

		m_sphere->CreateShaderResourceViews(m_resourceDescriptors->GetCpuHandle(static_cast<size_t>(DescriptorHeapIndex::SphereVertices)), m_resourceDescriptors->GetCpuHandle(static_cast<size_t>(DescriptorHeapIndex::SphereIndices)));

		CreateBottomLevelAS({ m_sphere->GetGeometryDesc() }, m_bottomLevelASBuffers);

		CreateTopLevelAS(false, m_topLevelASBuffers);

		commandList->Close();

		m_deviceResources->GetCommandQueue()->ExecuteCommandLists(1, CommandListCast(&commandList));

		m_deviceResources->WaitForGpu();
	}

	void CreateConstantBuffers() {
		m_sceneConstantBuffer = m_graphicsMemory->AllocateConstant(SceneConstant{ .MaxTraceRecursionDepth = MaxTraceRecursionDepth });

		const auto renderItemsSize = m_renderItems.size();

		m_objectConstantBuffer = m_graphicsMemory->Allocate(sizeof(ObjectConstant) * renderItemsSize);

		m_materialConstantBuffer = m_graphicsMemory->Allocate(sizeof(MaterialConstant) * renderItemsSize);

		for (size_t i = 0; i < renderItemsSize; i++) {
			reinterpret_cast<ObjectConstant*>(m_objectConstantBuffer.Memory())[i] = ObjectConstant{ .IsTextureUsed = m_renderItems[i].Texture != nullptr };

			reinterpret_cast<MaterialConstant*>(m_materialConstantBuffer.Memory())[i] = MaterialConstant(*m_renderItems[i].Material);
		}
	}

	void CreateShaderBindingTables() {
		m_shaderBindingTableGenerator = decltype(m_shaderBindingTableGenerator)();

		m_shaderBindingTableGenerator.AddRayGenerationProgram(RayGeneration,
			{
				reinterpret_cast<void*>(m_resourceDescriptors->GetFirstGpuHandle().ptr),
				reinterpret_cast<void*>(m_sceneConstantBuffer.GpuAddress())
			});

		m_shaderBindingTableGenerator.AddMissProgram(PrimaryRayMiss, { nullptr });

		for (const auto& renderItem : m_renderItems) {
			m_shaderBindingTableGenerator.AddHitGroup(SphereHitGroup,
				{
					reinterpret_cast<void*>(m_resourceDescriptors->GetGpuHandle(static_cast<size_t>(DescriptorHeapIndex::Scene)).ptr),
					reinterpret_cast<void*>(m_resourceDescriptors->GetGpuHandle(renderItem.VerticesSrvHeapIndex).ptr),
					reinterpret_cast<void*>(renderItem.Texture == nullptr ? 0 : m_resourceDescriptors->GetGpuHandle(renderItem.Texture->SrvHeapIndex).ptr),
					reinterpret_cast<void*>(m_sceneConstantBuffer.GpuAddress()),
					reinterpret_cast<ObjectConstant*>(m_objectConstantBuffer.GpuAddress()) + renderItem.ObjectConstantBufferIndex,
					reinterpret_cast<MaterialConstant*>(m_materialConstantBuffer.GpuAddress()) + renderItem.Material->ConstantBufferIndex
				});
		}

		DX::ThrowIfFailed(RaytracingHelpers::CreateUploadBuffer(m_deviceResources->GetD3DDevice(), m_shaderBindingTableGenerator.ComputeSBTSize(), m_shaderBindingTable.ReleaseAndGetAddressOf()));

		Microsoft::WRL::ComPtr<ID3D12StateObjectProperties> stateObjectProperties;
		DX::ThrowIfFailed(m_pipelineStateObject->QueryInterface(IID_PPV_ARGS(&stateObjectProperties)));

		m_shaderBindingTableGenerator.Generate(m_shaderBindingTable.Get(), stateObjectProperties.Get());
	}

	void CreateWindowSizeIndependentShaderResourceViews() {
		for (const auto& pair : m_textures) {
			const auto& texture = pair.second;
			DirectX::CreateShaderResourceView(m_deviceResources->GetD3DDevice(), texture->Resource.Get(), m_resourceDescriptors->GetCpuHandle(texture->SrvHeapIndex));
		}
	}

	void CreateWindowSizeDependentShaderResourceViews() {
		const auto device = m_deviceResources->GetD3DDevice();

		const auto defaultHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
		const auto outputSize = GetOutputSize();
		const auto resourceDesc = CD3DX12_RESOURCE_DESC::Tex2D(m_deviceResources->GetBackBufferFormat(), outputSize.cx, outputSize.cy, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		DX::ThrowIfFailed(device->CreateCommittedResource(&defaultHeapProperties, D3D12_HEAP_FLAG_NONE, &resourceDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(m_output.ReleaseAndGetAddressOf())));

		const D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{
			.Format = DXGI_FORMAT_UNKNOWN,
			.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D
		};
		device->CreateUnorderedAccessView(m_output.Get(), nullptr, &uavDesc, m_resourceDescriptors->GetCpuHandle(static_cast<size_t>(DescriptorHeapIndex::Output)));

		const D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{
			.Format = DXGI_FORMAT_UNKNOWN,
			.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE,
			.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
			.RaytracingAccelerationStructure = { m_topLevelASBuffers.Result->GetGPUVirtualAddress() }
		};
		device->CreateShaderResourceView(nullptr, &srvDesc, m_resourceDescriptors->GetCpuHandle(static_cast<size_t>(DescriptorHeapIndex::Scene)));
	}

	void UpdateCamera(const DirectX::GamePad::State(&gamepadStates)[DirectX::GamePad::MAX_PLAYER_COUNT], const DirectX::Mouse::State& mouseState, const DirectX::Mouse::State& lastMouseState) {
		using namespace DirectX;
		using Key = Keyboard::Keys;
		using MouseButtonState = Mouse::ButtonStateTracker::ButtonState;
		using GamepadButtonState = GamePad::ButtonStateTracker::ButtonState;

		const auto elapsedSeconds = static_cast<float>(m_stepTimer.GetElapsedSeconds());

		for (const auto& gamepadState : gamepadStates) {
			if (gamepadState.IsConnected()) {
				if (gamepadState.thumbSticks.leftX || gamepadState.thumbSticks.leftY
					|| gamepadState.thumbSticks.rightX || gamepadState.thumbSticks.rightY) {
					m_mouse->SetVisible(false);
				}

				m_orbitCamera.Update(elapsedSeconds * 4, gamepadState);
			}
		}

		constexpr Key Keys[]{ Key::W, Key::A, Key::S, Key::D, Key::Up, Key::Left, Key::Down, Key::Right };
		for (const auto key : Keys) if (m_keyboardStateTracker.IsKeyPressed(key)) m_mouse->SetVisible(false);

		if (m_mouseButtonStateTracker.leftButton == MouseButtonState::PRESSED) m_mouse->SetVisible(false);
		else if (m_mouseButtonStateTracker.leftButton == MouseButtonState::RELEASED
			|| (m_mouseButtonStateTracker.leftButton == MouseButtonState::UP
				&& (mouseState.x != lastMouseState.x || mouseState.y != lastMouseState.y))) {
			m_mouse->SetVisible(true);
		}

		if (mouseState.scrollWheelValue) {
			m_mouse->ResetScrollWheelValue();

			m_mouse->SetVisible(false);

			m_cameraRadius = std::clamp(m_cameraRadius - 0.5f * mouseState.scrollWheelValue / WHEEL_DELTA, MinCameraRadius, MaxCameraRadius);
			m_orbitCamera.SetRadius(m_cameraRadius, MinCameraRadius, MaxCameraRadius);
		}

		m_orbitCamera.Update(elapsedSeconds, *m_mouse, *m_keyboard);
	}

	void UpdateSceneConstantBuffer() {
		using namespace DirectX;

		auto& sceneConstant = *reinterpret_cast<SceneConstant*>(m_sceneConstantBuffer.Memory());

		const auto GetProjection = [&] {
			const auto outputSize = GetOutputSize();
			auto projection = m_orbitCamera.GetProjection();
			projection.r[0].m128_f32[0] = projection.r[1].m128_f32[1] / (static_cast<float>(outputSize.cx) / static_cast<float>(outputSize.cy));
			return projection;
		};
		XMStoreFloat4x4(&sceneConstant.ProjectionToWorld, XMMatrixTranspose(XMMatrixInverse(nullptr, m_orbitCamera.GetView() * GetProjection())));

		XMStoreFloat3(&sceneConstant.CameraPosition, m_orbitCamera.GetPosition());

		sceneConstant.AntiAliasingSampleCount = m_antiAliasingSampleCount;

		sceneConstant.FrameCount = m_stepTimer.GetFrameCount();
	}

	void UpdateRenderItems() {
		using namespace DirectX;

		const auto totalSeconds = static_cast<float>(m_stepTimer.GetTotalSeconds());

		for (auto& renderItem : m_renderItems) {
			if (renderItem.Name == Wave) {
				for (auto& transform : renderItem.Transforms) {
					if (transform.Type == Transform::Type::Translation) {
						transform.Translation = { 0, 0.5f * cos(totalSeconds * 1.1f + renderItem.Position.x) + 0.5f, 0 };
						break;
					}
				}
			}
			else {
				constexpr auto Mod = [](auto a, auto b) { return a - static_cast<int>(a / b) * b;  };

				constexpr auto EarthPeriod = 10;

				if (renderItem.Name == Earth) {
					const auto rotationY = XM_2PI * Mod(totalSeconds, EarthPeriod) / EarthPeriod;
					renderItem.Transforms = CreateTransformsForAstronomicalObject(0, rotationY, 0, { 2, 2, 2 });
				}
				else if (renderItem.Name == Moon) {
					constexpr auto MoonPeriod = EarthPeriod * 2;

					const auto revolutionY = -XM_2PI * Mod(totalSeconds, MoonPeriod) / MoonPeriod + XM_PIDIV2;
					renderItem.Transforms = CreateTransformsForAstronomicalObject(4, -revolutionY - XM_PIDIV2, revolutionY, { 0.5f, 0.5f, 0.5f });
				}
			}
		}
	}

	void TraceRays() {
		const auto commandList = m_deviceResources->GetCommandList();

		CreateTopLevelAS(true, m_topLevelASBuffers);

		ID3D12DescriptorHeap* const descriptorHeaps[]{ m_resourceDescriptors->Heap() };
		commandList->SetDescriptorHeaps(ARRAYSIZE(descriptorHeaps), descriptorHeaps);

		commandList->SetPipelineState1(m_pipelineStateObject.Get());

		const auto rayGenerationSectionSize = m_shaderBindingTableGenerator.GetRayGenerationSectionSize(),
			missSectionSize = m_shaderBindingTableGenerator.GetMissSectionSize(),
			hitGroupSectionSize = m_shaderBindingTableGenerator.GetHitGroupSectionSize();

		const auto rayGenerationShaderRecordStartAddress = m_shaderBindingTable->GetGPUVirtualAddress(),
			missShaderTableStartAddress = rayGenerationShaderRecordStartAddress + rayGenerationSectionSize,
			hitGroupTableStartAddress = missShaderTableStartAddress + missSectionSize;

		const auto outputSize = GetOutputSize();

		const D3D12_DISPATCH_RAYS_DESC raysDesc{
			.RayGenerationShaderRecord = {
				.StartAddress = rayGenerationShaderRecordStartAddress,
				.SizeInBytes = rayGenerationSectionSize
			},
			.MissShaderTable = {
				.StartAddress = missShaderTableStartAddress,
				.SizeInBytes = missSectionSize,
				.StrideInBytes = m_shaderBindingTableGenerator.GetMissEntrySize()
			},
			.HitGroupTable = {
				.StartAddress = hitGroupTableStartAddress,
				.SizeInBytes = hitGroupSectionSize,
				.StrideInBytes = m_shaderBindingTableGenerator.GetHitGroupEntrySize()
			},
			.Width = static_cast<UINT>(outputSize.cx),
			.Height = static_cast<UINT>(outputSize.cy),
			.Depth = 1
		};

		commandList->DispatchRays(&raysDesc);
	}
};
