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

#include "DDSTextureLoader.h"

#include "BottomLevelASGenerator.h"
#include "TopLevelASGenerator.h"
#include "ShaderBindingTableGenerator.h"

#include "RaytracingGeometryHelpers.h"

#include "Material.h"
#include "Texture.h"

#include "Random.h"

#include "MyPhysX.h"

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

		BuildTextures();

		BuildRenderItems();

		m_deviceResources->RegisterDeviceNotify(this);

		m_deviceResources->SetWindow(hWnd, static_cast<int>(outputSize.cx), static_cast<int>(outputSize.cy));

		m_deviceResources->CreateDeviceResources();
		CreateDeviceDependentResources();

		m_deviceResources->CreateWindowSizeDependentResources();
		CreateWindowSizeDependentResources();

		m_mouse->SetWindow(hWnd);

		m_orbitCamera.SetRadius(m_cameraRadius, MinCameraRadius, MaxCameraRadius);
	}

	~D3DApp() { m_deviceResources->WaitForGpu(); }

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

		m_objectConstantBuffer.Reset();
		m_sceneConstantBuffer.Reset();

		m_topLevelASBuffers = {};
		m_bottomLevelASBuffers = {};

		m_sphere.reset();

		for (const auto& renderItem : m_renderItems) renderItem.Texture->Resource.Reset();

		for (const auto& pair : m_textures) pair.second->Resource.Reset();

		m_resourceDescriptors.reset();

		m_pipelineStateObject.Reset();

		m_globalRootSignature.Reset();

		m_graphicsMemory.reset();
	}

	void OnDeviceRestored() override {
		CreateDeviceDependentResources();

		CreateWindowSizeDependentResources();
	}

private:
	enum class Descriptors {
		Output,
		SphereVertices, SphereIndices,
		EarthImageTexture, MoonImageTexture,
		Count
	};

	struct RenderItem {
		std::string Name;
		std::wstring HitGroup;
		size_t VerticesDescriptorHeapIndex = SIZE_MAX, ObjectConstantBufferIndex = SIZE_MAX;
		std::shared_ptr<Material> Material;
		std::shared_ptr<Texture> Texture;
		physx::PxShape* Shape{};
	};

	struct alignas(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT) SceneConstant {
		DirectX::XMFLOAT4X4 ProjectionToWorld;
		DirectX::XMFLOAT3 CameraPosition;
		UINT AntiAliasingSampleCount;
		UINT FrameCount;
	};

	struct alignas(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT) ObjectConstant {
		BOOL IsImageTextureUsed;
		DirectX::XMFLOAT3 Padding;
		MaterialBase Material;
	};

	static constexpr struct {
		LPCSTR
			Earth = "Earth", Moon = "Moon", Star = "Star",
			HarmonicOscillator = "HarmonicOscillator";
	} Objects{};

	static constexpr struct { LPCWSTR RayGeneration = L"RayGeneration", PrimaryRayMiss = L"PrimaryRayMiss"; } ShaderEntryPoints{};

	static constexpr struct { LPCWSTR PrimaryRayHitGroup = L"PrimaryRayHitGroup"; } ShaderSubobjects{};

	const std::unique_ptr<DirectX::GamePad> m_gamepad = std::make_unique<decltype(m_gamepad)::element_type>();
	const std::unique_ptr<DirectX::Keyboard> m_keyboard = std::make_unique<decltype(m_keyboard)::element_type>();
	const std::unique_ptr<DirectX::Mouse> m_mouse = std::make_unique<decltype(m_mouse)::element_type>();

	DirectX::GamePad::ButtonStateTracker m_gamepadButtonStateTrackers[DirectX::GamePad::MAX_PLAYER_COUNT];
	DirectX::Keyboard::KeyboardStateTracker m_keyboardStateTracker;
	DirectX::Mouse::ButtonStateTracker m_mouseButtonStateTracker;

	std::unique_ptr<DX::DeviceResources> m_deviceResources = std::make_unique<decltype(m_deviceResources)::element_type>();
	std::unique_ptr<DirectX::GraphicsMemory> m_graphicsMemory;

	DX::StepTimer m_stepTimer;

	static constexpr float MinCameraRadius = 1, MaxCameraRadius = 100;
	float m_cameraRadius = 15;
	DX::OrbitCamera m_orbitCamera;

	UINT m_antiAliasingSampleCount{};

	std::map<std::string, std::shared_ptr<Texture>> m_textures;

	std::vector<RenderItem> m_renderItems;

	Microsoft::WRL::ComPtr<ID3D12RootSignature> m_globalRootSignature;

	Microsoft::WRL::ComPtr<ID3D12StateObject> m_pipelineStateObject;

	std::unique_ptr<DirectX::DescriptorHeap> m_resourceDescriptors;

	DirectX::GraphicsResource m_sceneConstantBuffer, m_objectConstantBuffer;

	static constexpr float SphereRadius = 0.5f;
	std::shared_ptr<RaytracingHelpers::Triangles<DirectX::VertexPositionNormalTexture, UINT16>> m_sphere;

	RaytracingHelpers::AccelerationStructureBuffers m_bottomLevelASBuffers, m_topLevelASBuffers;

	nv_helpers_dx12::ShaderBindingTableGenerator m_shaderBindingTableGenerator;
	Microsoft::WRL::ComPtr<ID3D12Resource> m_shaderBindingTable;

	Microsoft::WRL::ComPtr<ID3D12Resource> m_output;

	MyPhysX m_myPhysX;

	struct Sphere {
		bool IsGravityEnabled;
		physx::PxVec3 Position;
		physx::PxReal Radius, RotationPeriod, OrbitalPeriod, Mass;
	} m_Moon{
		.Position = { 0, 4, 4 },
		.Radius = SphereRadius * 0.5f,
		.OrbitalPeriod = 10
	}, m_Earth{
		.Position = { 0, m_Moon.Position.y, 0 },
		.Radius = SphereRadius * 2,
		.RotationPeriod = 15,
		.Mass = PhysicsHelpers::Gravity::CalculateMass((m_Moon.Position - m_Earth.Position).magnitude(), m_Moon.OrbitalPeriod)
	}, m_star{
		.Position = { 0, -50.0f - 0.1f, 0 },
		.Radius = 50
	};

	const struct { physx::PxReal PositionY = 0.5f, Period = 3; } m_spring;

	static void CopyOutputToRenderTarget(ID3D12GraphicsCommandList* pCommandList, ID3D12Resource* pOutput, ID3D12Resource* pRenderTarget) {
		using namespace DirectX;

		TransitionResource(pCommandList, pRenderTarget, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
		TransitionResource(pCommandList, pOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);

		pCommandList->CopyResource(pRenderTarget, pOutput);

		TransitionResource(pCommandList, pOutput, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		TransitionResource(pCommandList, pRenderTarget, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
	}

	static auto CreateSphere(ID3D12Device* pDevice, float radius, size_t tessellation) {
		DirectX::GeometricPrimitive::VertexCollection vertices;
		DirectX::GeometricPrimitive::IndexCollection indices;
		DirectX::GeometricPrimitive::CreateGeoSphere(vertices, indices, radius * 2, tessellation, false);
		return RaytracingHelpers::Triangles(pDevice, vertices, indices, D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE);
	}

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

		DispatchRays();

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

		ToggleGravities();

		m_myPhysX.Tick(1.0f / 60);

		PIXEndEvent();
	}

	void CreateDeviceDependentResources() {
		const auto device = m_deviceResources->GetD3DDevice();

		m_graphicsMemory = std::make_unique<decltype(m_graphicsMemory)::element_type>(device);

		LoadTextures();

		CreateRootSignatures();

		CreatePipelineStateObjects();

		CreateDescriptorHeaps();

		CreateGeometries();

		CreateAccelerationStructures();

		CreateConstantBuffers();

		CreateShaderBindingTables();

		CreateDeviceDependentShaderResourceViews();
	}

	void CreateWindowSizeDependentResources() { CreateWindowSizeDependentShaderResourceViews(); }

	void BuildTextures() {
		if (!m_textures.empty()) return;

		const auto path = std::filesystem::path(*__wargv).replace_filename(L"Textures\\").wstring();

		constexpr struct {
			LPCSTR Name;
			LPCWSTR Path;
			Descriptors Descriptor;
		} ImageTextures[]{
			{ Objects.Earth, L"Earth.dds", Descriptors::EarthImageTexture },
			{ Objects.Moon, L"Moon.dds", Descriptors::MoonImageTexture }
		};

		for (const auto& imageTexture : ImageTextures) {
			const auto texture = std::make_shared<Texture>();
			texture->DescriptorHeapIndex = static_cast<size_t>(imageTexture.Descriptor);
			texture->Path = path + imageTexture.Path;
			m_textures[imageTexture.Name] = texture;
		}
	}

	void LoadTextures() {
		using namespace DirectX;

		const auto device = m_deviceResources->GetD3DDevice();

		ResourceUploadBatch resourceUploadBatch(device);
		resourceUploadBatch.Begin();

		for (const auto& pair : m_textures) {
			const auto& texture = pair.second;
			DX::ThrowIfFailed(CreateDDSTextureFromFile(device, resourceUploadBatch, texture->Path.c_str(), texture->Resource.ReleaseAndGetAddressOf()));
		}

		resourceUploadBatch.End(m_deviceResources->GetCommandQueue()).wait();
	}

	void BuildRenderItems() {
		using namespace DirectX;
		using namespace physx;
		using namespace PhysicsHelpers;

		if (!m_renderItems.empty()) return;

		const auto AddRenderItem = [&](RenderItem& renderItem, const MaterialBase& material, const PxSphereGeometry& geometry, const PxVec3& position) {
			renderItem.HitGroup = ShaderSubobjects.PrimaryRayHitGroup;

			renderItem.VerticesDescriptorHeapIndex = static_cast<size_t>(Descriptors::SphereVertices);

			renderItem.ObjectConstantBufferIndex = m_renderItems.size();

			renderItem.Material = std::make_shared<Material>(material, renderItem.ObjectConstantBufferIndex);

			const auto rigidDynamic = m_myPhysX.AddRigidDynamic(geometry, PxTransform(position));

			PxRigidBodyExt::updateMassAndInertia(*rigidDynamic, 1);

			rigidDynamic->setAngularDamping(0);
			rigidDynamic->setLinearDamping(0);

			rigidDynamic->getShapes(&renderItem.Shape, sizeof(renderItem.Shape));

			m_renderItems.emplace_back(renderItem);

			return rigidDynamic;
		};

		{
			const struct {
				PxVec3 Position;
				MaterialBase Material;
			} objects[]{
				{
					{ -2, SphereRadius, 0 },
					MaterialBase::CreateLambertian({ 0.1f, 0.2f, 0.5f, 1 })
				},
				{
					{ 0, SphereRadius, 0 },
					MaterialBase::CreateDielectric({ 1, 1, 1, 1 }, 1.5f)
				},
				{
					{ 2, SphereRadius, 0 },
					MaterialBase::CreateMetal({ 0.7f, 0.6f, 0.5f, 1 }, 0.2f)
				}
			};
			for (const auto& object : objects) {
				RenderItem renderItem;
				AddRenderItem(renderItem, object.Material, PxSphereGeometry(SphereRadius), object.Position);
			}

			for (int i = -10; i < 10; i++) {
				for (int j = -10; j < 10; j++) {
					constexpr auto A = 0.5f;
					const auto ω = PxTwoPi / m_spring.Period;

					PxVec3 position;
					position.x = i + 0.7f * Random::Float();
					position.y = m_spring.PositionY + SimpleHarmonicMotion::Spring::CalculateDisplacement(A, ω, 0.0f, position.x);
					position.z = j - 0.7f * Random::Float();

					bool collision = false;
					for (const auto& sphere : objects) {
						if ((position - sphere.Position).magnitude() < 1) {
							collision = true;
							break;
						}
					}
					if (collision) continue;

					MaterialBase material;
					const auto randomValue = Random::Float();
					if (randomValue < 0.5f) {
						material = MaterialBase::CreateLambertian(Random::Float4());
					}
					else if (randomValue < 0.75f) {
						material = MaterialBase::CreateMetal(Random::Float4(0.5f, 1), Random::Float(0, 0.5f));
					}
					else {
						material = MaterialBase::CreateDielectric(Random::Float4(), 1.5f);
					}

					RenderItem renderItem;

					renderItem.Name = Objects.HarmonicOscillator;

					const auto rigidDynamic = AddRenderItem(renderItem, material, PxSphereGeometry(SphereRadius * 0.15f), position);

					rigidDynamic->setLinearVelocity(PxVec3(0, SimpleHarmonicMotion::Spring::CalculateVelocity(A, ω, 0.0f, position.x), 0));
				}
			}
		}

		{
			const struct {
				LPCSTR Name;
				Sphere& Sphere;
				MaterialBase Material;
			} objects[]{
				{
					Objects.Moon,
					m_Moon,
					MaterialBase::CreateLambertian({ 0.5f, 0.5f, 0.5f, 1 }),
				},
				{
					Objects.Earth,
					m_Earth,
					MaterialBase::CreateLambertian({ 0.5f, 0.5f, 0.5f, 1 })
				},
				{
					Objects.Star,
					m_star,
					MaterialBase::CreateMetal({ 0.5f, 0.5f, 0.5f, 1 }, 0)
				}
			};
			for (const auto& object : objects) {
				RenderItem renderItem;

				renderItem.Name = object.Name;

				if (m_textures.contains(object.Name)) renderItem.Texture = m_textures[object.Name];

				const auto rigidDynamic = AddRenderItem(renderItem, object.Material, PxSphereGeometry(object.Sphere.Radius), object.Sphere.Position);

				if (renderItem.Name == Objects.Moon) {
					const auto x = m_Earth.Position - object.Sphere.Position;
					const auto magnitude = x.magnitude();
					const auto normalized = x / magnitude;
					const auto linearSpeed = Gravity::CalculateFirstCosmicSpeed(m_Earth.Mass, magnitude);
					rigidDynamic->setLinearVelocity(linearSpeed * PxVec3(-normalized.z, 0, normalized.x));
					rigidDynamic->setAngularVelocity({ 0, linearSpeed / magnitude, 0 });
				}
				else if (renderItem.Name == Objects.Earth) {
					rigidDynamic->setAngularVelocity({ 0, PxTwoPi / object.Sphere.RotationPeriod, 0 });

					PxRigidBodyExt::setMassAndUpdateInertia(*rigidDynamic, &object.Sphere.Mass, 1);
				}
				else if (renderItem.Name == Objects.Star) {
					rigidDynamic->setMass(0);
				}
			}
		}
	}

	void CreateRootSignatures() {
		const auto device = m_deviceResources->GetD3DDevice();

		DX::ThrowIfFailed(device->CreateRootSignature(0, g_pRaytracing, ARRAYSIZE(g_pRaytracing), IID_PPV_ARGS(m_globalRootSignature.ReleaseAndGetAddressOf())));
	}

	void CreatePipelineStateObjects() {
		const auto device = m_deviceResources->GetD3DDevice();

		CD3DX12_STATE_OBJECT_DESC stateObjectDesc(D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE);

		const auto dxilLibrary = stateObjectDesc.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();

		const CD3DX12_SHADER_BYTECODE shader(g_pRaytracing, ARRAYSIZE(g_pRaytracing));
		dxilLibrary->SetDXILLibrary(&shader);

		dxilLibrary->DefineExport(L"RaytracingShaderConfig");
		dxilLibrary->DefineExport(L"RaytracingPipelineConfig");

		dxilLibrary->DefineExport(L"GlobalRootSignature");
		dxilLibrary->DefineExport(L"LocalRootSignature");

		dxilLibrary->DefineExport(ShaderEntryPoints.RayGeneration);

		dxilLibrary->DefineExport(ShaderEntryPoints.PrimaryRayMiss);
		dxilLibrary->DefineExport(L"PrimaryRayClosestHit");
		dxilLibrary->DefineExport(ShaderSubobjects.PrimaryRayHitGroup);
		dxilLibrary->DefineExport(L"PrimaryRayLocalRootSignatureAssociation");

		DX::ThrowIfFailed(device->CreateStateObject(stateObjectDesc, IID_PPV_ARGS(m_pipelineStateObject.ReleaseAndGetAddressOf())));
	}

	void CreateDescriptorHeaps() {
		const auto device = m_deviceResources->GetD3DDevice();

		m_resourceDescriptors = std::make_unique<DirectX::DescriptorHeap>(device, static_cast<size_t>(Descriptors::Count));
	}

	void CreateGeometries() {
		const auto device = m_deviceResources->GetD3DDevice();

		m_sphere = std::make_shared<decltype(m_sphere)::element_type>(CreateSphere(device, SphereRadius, 6));
	}

	void CreateBottomLevelAS(const std::vector<D3D12_RAYTRACING_GEOMETRY_DESC>& geometryDescs, RaytracingHelpers::AccelerationStructureBuffers& buffers) {
		const auto device = m_deviceResources->GetD3DDevice();

		nv_helpers_dx12::BottomLevelASGenerator bottomLevelASGenerator(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE);

		for (const auto& geometryDesc : geometryDescs) bottomLevelASGenerator.AddGeometry(geometryDesc);

		UINT64 scratchSize, resultSize;
		bottomLevelASGenerator.ComputeASBufferSizes(device, scratchSize, resultSize);

		buffers = RaytracingHelpers::AccelerationStructureBuffers(device, scratchSize, resultSize);

		bottomLevelASGenerator.Generate(m_deviceResources->GetCommandList(), buffers.Scratch.Get(), buffers.Result.Get());
	}

	void CreateTopLevelAS(bool updateOnly, RaytracingHelpers::AccelerationStructureBuffers& buffers) {
		using namespace DirectX;
		using namespace physx;

		nv_helpers_dx12::TopLevelASGenerator topLevelASGenerator(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE);

		for (UINT size = static_cast<UINT>(m_renderItems.size()), i = 0; i < size; i++) {
			auto& renderItem = m_renderItems[i];

			const auto& shape = *renderItem.Shape;

			PxVec3 scaling;
			const auto geometry = shape.getGeometry();
			switch (shape.getGeometryType()) {
			case PxGeometryType::eSPHERE: scaling = PxVec3(geometry.sphere().radius / SphereRadius); break;
			default: throw;
			}

			if (updateOnly) UpdateRenderItem(renderItem);

			PxMat44 world(PxShapeExt::getGlobalPose(shape, *shape.getActor()));
			world.scale(PxVec4(scaling, 1));
			topLevelASGenerator.AddInstance(m_bottomLevelASBuffers.Result->GetGPUVirtualAddress(), XMLoadFloat4x4(reinterpret_cast<const XMFLOAT4X4*>(world.front())), i, i);
		}

		const auto device = m_deviceResources->GetD3DDevice();

		const auto commandList = m_deviceResources->GetCommandList();

		if (updateOnly) {
			const auto uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(buffers.Result.Get());
			commandList->ResourceBarrier(1, &uavBarrier);
		}
		else {
			UINT64 scratchSize, resultSize, instanceDescsSize;
			topLevelASGenerator.ComputeASBufferSizes(device, scratchSize, resultSize, instanceDescsSize);
			buffers = RaytracingHelpers::AccelerationStructureBuffers(device, scratchSize, resultSize, instanceDescsSize);
		}

		topLevelASGenerator.Generate(commandList, buffers.Scratch.Get(), buffers.Result.Get(), buffers.InstanceDesc.Get(), updateOnly ? buffers.Result.Get() : nullptr);
	}

	void CreateAccelerationStructures() {
		const auto commandList = m_deviceResources->GetCommandList();

		commandList->Reset(m_deviceResources->GetCommandAllocator(), nullptr);

		CreateBottomLevelAS({ m_sphere->GetGeometryDesc() }, m_bottomLevelASBuffers);

		CreateTopLevelAS(false, m_topLevelASBuffers);

		commandList->Close();

		m_deviceResources->GetCommandQueue()->ExecuteCommandLists(1, CommandListCast(&commandList));

		m_deviceResources->WaitForGpu();
	}

	void CreateConstantBuffers() {
		m_sceneConstantBuffer = m_graphicsMemory->Allocate(sizeof(SceneConstant));

		const auto renderItemsSize = m_renderItems.size();

		m_objectConstantBuffer = m_graphicsMemory->Allocate(sizeof(ObjectConstant) * renderItemsSize);

		for (size_t i = 0; i < renderItemsSize; i++) {
			reinterpret_cast<ObjectConstant*>(m_objectConstantBuffer.Memory())[i] = {
				.IsImageTextureUsed = m_renderItems[i].Texture != nullptr,
				.Material = *m_renderItems[i].Material
			};
		}
	}

	void CreateShaderBindingTables() {
		m_shaderBindingTableGenerator = {};

		m_shaderBindingTableGenerator.AddRayGenerationProgram(ShaderEntryPoints.RayGeneration, { nullptr });

		m_shaderBindingTableGenerator.AddMissProgram(ShaderEntryPoints.PrimaryRayMiss, { nullptr });

		for (const auto& renderItem : m_renderItems) {
			m_shaderBindingTableGenerator.AddHitGroup(
				renderItem.HitGroup.c_str(),
				{
					reinterpret_cast<void*>(m_resourceDescriptors->GetGpuHandle(renderItem.VerticesDescriptorHeapIndex).ptr),
					reinterpret_cast<void*>(renderItem.Texture == nullptr ? 0 : m_resourceDescriptors->GetGpuHandle(renderItem.Texture->DescriptorHeapIndex).ptr),
					reinterpret_cast<ObjectConstant*>(m_objectConstantBuffer.GpuAddress()) + renderItem.ObjectConstantBufferIndex
				}
			);
		}

		DX::ThrowIfFailed(RaytracingHelpers::CreateUploadBuffer(m_deviceResources->GetD3DDevice(), m_shaderBindingTableGenerator.ComputeSBTSize(), m_shaderBindingTable.ReleaseAndGetAddressOf()));

		Microsoft::WRL::ComPtr<ID3D12StateObjectProperties> stateObjectProperties;
		DX::ThrowIfFailed(m_pipelineStateObject->QueryInterface(IID_PPV_ARGS(&stateObjectProperties)));

		m_shaderBindingTableGenerator.Generate(m_shaderBindingTable.Get(), stateObjectProperties.Get());
	}

	void CreateDeviceDependentShaderResourceViews() {
		for (const auto& pair : m_textures) {
			const auto& texture = pair.second;
			DirectX::CreateShaderResourceView(m_deviceResources->GetD3DDevice(), texture->Resource.Get(), m_resourceDescriptors->GetCpuHandle(texture->DescriptorHeapIndex));
		}

		m_sphere->CreateShaderResourceViews(m_resourceDescriptors->GetCpuHandle(static_cast<size_t>(Descriptors::SphereVertices)), m_resourceDescriptors->GetCpuHandle(static_cast<size_t>(Descriptors::SphereIndices)));
	}

	void CreateWindowSizeDependentShaderResourceViews() {
		const auto device = m_deviceResources->GetD3DDevice();

		const auto defaultHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
		const auto outputSize = GetOutputSize();
		const auto resourceDesc = CD3DX12_RESOURCE_DESC::Tex2D(m_deviceResources->GetBackBufferFormat(), outputSize.cx, outputSize.cy, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		DX::ThrowIfFailed(device->CreateCommittedResource(&defaultHeapProperties, D3D12_HEAP_FLAG_NONE, &resourceDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(m_output.ReleaseAndGetAddressOf())));

		const D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{ .ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D };
		device->CreateUnorderedAccessView(m_output.Get(), nullptr, &uavDesc, m_resourceDescriptors->GetCpuHandle(static_cast<size_t>(Descriptors::Output)));
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

	void ToggleGravities() {
		using namespace DirectX;
		using Key = Keyboard::Keys;
		using GamepadButtonState = GamePad::ButtonStateTracker::ButtonState;

		for (const auto& gamepadButtonStateTracker : m_gamepadButtonStateTrackers) {
			if (gamepadButtonStateTracker.a == GamepadButtonState::PRESSED) m_Earth.IsGravityEnabled = !m_Earth.IsGravityEnabled;
			if (gamepadButtonStateTracker.b == GamepadButtonState::PRESSED) m_star.IsGravityEnabled = !m_star.IsGravityEnabled;
		}

		if (m_keyboardStateTracker.IsKeyPressed(Key::G)) m_Earth.IsGravityEnabled = !m_Earth.IsGravityEnabled;
		if (m_keyboardStateTracker.IsKeyPressed(Key::H)) m_star.IsGravityEnabled = !m_star.IsGravityEnabled;
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

	void UpdateRenderItem(RenderItem& renderItem) {
		using namespace physx;
		using namespace PhysicsHelpers;

		const auto& shape = *renderItem.Shape;

		const auto rigidBody = shape.getActor()->is<PxRigidBody>();
		if (rigidBody == nullptr) return;

		const auto mass = rigidBody->getMass();
		if (mass == 0) return;

		const auto& position = PxShapeExt::getGlobalPose(shape, *rigidBody).p;

		if (renderItem.Name == Objects.HarmonicOscillator) {
			const auto k = SimpleHarmonicMotion::Spring::CalculateConstant(mass, m_spring.Period);
			const PxVec3 x(0, position.y - m_spring.PositionY, 0);
			rigidBody->addForce(-k * x);
		}

		if ((m_Earth.IsGravityEnabled && renderItem.Name != Objects.Earth)
			|| renderItem.Name == Objects.Moon) {
			const auto x = m_Earth.Position - position;
			const auto magnitude = x.magnitude();
			const auto normalized = x / magnitude;
			rigidBody->addForce(Gravity::CalculateAccelerationMagnitude(m_Earth.Mass, magnitude) * normalized, PxForceMode::eACCELERATION);
		}

		if (m_star.IsGravityEnabled && renderItem.Name != Objects.Star) {
			const auto x = m_star.Position - position;
			const auto normalized = x.getNormalized();
			rigidBody->addForce(10 * normalized, PxForceMode::eACCELERATION);
		}
	}

	void DispatchRays() {
		const auto commandList = m_deviceResources->GetCommandList();

		CreateTopLevelAS(true, m_topLevelASBuffers);

		ID3D12DescriptorHeap* const descriptorHeaps[]{ m_resourceDescriptors->Heap() };
		commandList->SetDescriptorHeaps(ARRAYSIZE(descriptorHeaps), descriptorHeaps);

		commandList->SetComputeRootSignature(m_globalRootSignature.Get());

		commandList->SetComputeRootDescriptorTable(0, m_resourceDescriptors->GetGpuHandle(static_cast<size_t>(Descriptors::Output)));

		commandList->SetComputeRootShaderResourceView(1, m_topLevelASBuffers.Result->GetGPUVirtualAddress());

		commandList->SetComputeRootConstantBufferView(2, m_sceneConstantBuffer.GpuAddress());

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
