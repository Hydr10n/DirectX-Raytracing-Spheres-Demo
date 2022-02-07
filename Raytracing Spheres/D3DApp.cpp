#pragma warning(disable: 26812)

#include "D3DApp.h"

#include "DirectXHelpers.h"

#include "ResourceUploadBatch.h"

#include "GeometricPrimitive.h"

#include "DDSTextureLoader.h"
#include "WICTextureLoader.h"

#include "BottomLevelAccelerationStructureGenerator.h"
#include "TopLevelAccelerationStructureGenerator.h"

#include "Random.h"

#include "Shaders/Raytracing.hlsl.h"

#include <filesystem>

using namespace DirectX;
using namespace DX;
using namespace PhysicsHelpers;
using namespace physx;
using namespace RaytracingHelpers;
using namespace std;

using Key = Keyboard::Keys;
using GamepadButtonState = GamePad::ButtonStateTracker::ButtonState;

struct DescriptorHeapIndices {
	enum {
		Output,
		SphereVertices, SphereIndices,
		MoonImageTexture, MoonNormalTexture,
		EarthImageTexture, EarthNormalTexture,
		Count
	};
};

struct alignas(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT) SceneConstant {
	XMFLOAT4X4 ProjectionToWorld;
	XMFLOAT3 CameraPosition;
	UINT AntiAliasingSampleCount, FrameCount;
};

struct TextureFlags { enum { Image = 0x1, Normal = 0x2 }; };

struct alignas(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT) ObjectConstant {
	UINT TextureFlags;
	XMFLOAT3 Padding;
	MaterialBase Material;
};

static constexpr struct {
	LPCSTR
		Earth = "Earth", Moon = "Moon", Star = "Star",
		HarmonicOscillator = "HarmonicOscillator";
} Objects{};

static constexpr struct { LPCWSTR RayGeneration = L"RayGeneration", PrimaryRayMiss = L"PrimaryRayMiss"; } ShaderEntryPoints{};

static constexpr struct { LPCWSTR PrimaryRayHitGroup = L"PrimaryRayHitGroup"; } ShaderSubobjects{};

D3DApp::D3DApp(HWND hWnd, const SIZE& outputSize) noexcept(false) {
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
	m_mouse->SetMode(Mouse::MODE_RELATIVE);

	m_camera.SetPosition({ 0, 0, 15 });
}

D3DApp::~D3DApp() { m_deviceResources->WaitForGpu(); }

void D3DApp::Tick() {
	m_stepTimer.Tick([&] { Update(); });

	Render();
}

void D3DApp::OnWindowSizeChanged(const SIZE& size) {
	if (!m_deviceResources->WindowSizeChanged(static_cast<int>(size.cx), static_cast<int>(size.cy))) return;

	CreateWindowSizeDependentResources();
}

void D3DApp::OnActivated() {}

void D3DApp::OnDeactivated() {}

void D3DApp::OnResuming() {
	m_stepTimer.ResetElapsedTime();

	m_gamepad->Resume();
}

void D3DApp::OnSuspending() { m_gamepad->Suspend(); }

void D3DApp::OnDeviceLost() {
	m_output.Reset();

	m_shaderBindingTable.Reset();

	m_objectConstantBuffer.Reset();
	m_sceneConstantBuffer.Reset();

	m_topLevelAccelerationStructureBuffers = {};
	m_sphereBottomLevelAccelerationStructureBuffers = {};

	m_sphere.reset();

	for (auto& pair : m_textures) for (auto& pair1 : pair.second) pair1.second.Resource.Reset();

	m_resourceDescriptors.reset();

	m_pipelineStateObject.Reset();

	m_globalRootSignature.Reset();

	m_graphicsMemory.reset();
}

void D3DApp::OnDeviceRestored() {
	CreateDeviceDependentResources();

	CreateWindowSizeDependentResources();
}

void D3DApp::Update() {
	PIXBeginEvent(PIX_COLOR_DEFAULT, L"Update");

	ProcessInput();

	UpdateSceneConstantBuffer();

	if (m_isRunning) m_myPhysX.Tick(1.0f / 60);

	PIXEndEvent();
}

void D3DApp::Render() {
	if (!m_stepTimer.GetFrameCount()) return;

	m_deviceResources->Prepare();

	Clear();

	const auto commandList = m_deviceResources->GetCommandList();

	PIXBeginEvent(commandList, PIX_COLOR_DEFAULT, L"Render");

	DispatchRays();

	{
		const auto output = m_output.Get(), renderTarget = m_deviceResources->GetRenderTarget();

		TransitionResource(commandList, renderTarget, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
		TransitionResource(commandList, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);

		commandList->CopyResource(renderTarget, output);

		TransitionResource(commandList, output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		TransitionResource(commandList, renderTarget, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
	}

	PIXEndEvent(commandList);

	PIXBeginEvent(PIX_COLOR_DEFAULT, L"Present");

	m_deviceResources->Present(D3D12_RESOURCE_STATE_PRESENT);

	m_graphicsMemory->Commit(m_deviceResources->GetCommandQueue());

	m_deviceResources->WaitForGpu();

	PIXEndEvent();
}

void D3DApp::Clear() {
	const auto commandList = m_deviceResources->GetCommandList();

	PIXBeginEvent(commandList, PIX_COLOR_DEFAULT, L"Clear");

	const auto rtvDescriptor = m_deviceResources->GetRenderTargetView(), dsvDescriptor = m_deviceResources->GetDepthStencilView();
	commandList->OMSetRenderTargets(1, &rtvDescriptor, FALSE, &dsvDescriptor);
	commandList->ClearRenderTargetView(rtvDescriptor, Colors::CornflowerBlue, 0, nullptr);
	commandList->ClearDepthStencilView(dsvDescriptor, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1, 0, 0, nullptr);

	const auto viewport = m_deviceResources->GetScreenViewport();
	const auto scissorRect = m_deviceResources->GetScissorRect();
	commandList->RSSetViewports(1, &viewport);
	commandList->RSSetScissorRects(1, &scissorRect);

	PIXEndEvent(commandList);
}

void D3DApp::CreateDeviceDependentResources() {
	const auto device = m_deviceResources->GetD3DDevice();

	m_graphicsMemory = make_unique<decltype(m_graphicsMemory)::element_type>(device);

	CreateRootSignatures();

	CreatePipelineStateObjects();

	CreateDescriptorHeaps();

	CreateGeometries();

	CreateAccelerationStructures();

	CreateConstantBuffers();

	CreateShaderBindingTables();

	LoadTextures();
}

void D3DApp::CreateWindowSizeDependentResources() {
	const auto outputSize = GetOutputSize();
	m_camera.SetLens(XM_PIDIV4, static_cast<float>(outputSize.cx) / static_cast<float>(outputSize.cy), 1e-1f, 1e5f);

	const auto device = m_deviceResources->GetD3DDevice();

	const auto defaultHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

	const D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{ .ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D };

	const auto tex2DDesc = CD3DX12_RESOURCE_DESC::Tex2D(m_deviceResources->GetBackBufferFormat(), outputSize.cx, outputSize.cy, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

	ThrowIfFailed(device->CreateCommittedResource(&defaultHeapProperties, D3D12_HEAP_FLAG_NONE, &tex2DDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(m_output.ReleaseAndGetAddressOf())));

	device->CreateUnorderedAccessView(m_output.Get(), nullptr, &uavDesc, m_resourceDescriptors->GetCpuHandle(DescriptorHeapIndices::Output));
}

void D3DApp::BuildTextures() {
	if (!m_textures.empty()) return;

	const auto path = filesystem::path(*__wargv).replace_filename(L"Textures\\").wstring();

	constexpr struct {
		LPCSTR Name;
		TextureType Type;
		LPCWSTR Path;
		size_t DescriptorHeapIndex;
	} Textures[]{
		{ Objects.Moon, TextureType::Image, L"Moon.jpg", DescriptorHeapIndices::MoonImageTexture },
		{ Objects.Moon, TextureType::Normal, L"Moon_Normal.jpg", DescriptorHeapIndices::MoonNormalTexture },
		{ Objects.Earth, TextureType::Image, L"Earth.jpg", DescriptorHeapIndices::EarthImageTexture },
		{ Objects.Earth, TextureType::Normal, L"Earth_Normal.jpg", DescriptorHeapIndices::EarthNormalTexture }
	};

	for (const auto& texture : Textures) {
		m_textures[texture.Name][texture.Type] = Texture{
			.DescriptorHeapIndex = texture.DescriptorHeapIndex,
			.Path = path + texture.Path
		};
	}
}

void D3DApp::LoadTextures() {
	const auto device = m_deviceResources->GetD3DDevice();

	ResourceUploadBatch resourceUploadBatch(device);
	resourceUploadBatch.Begin();

	for (auto& pair : m_textures) {
		for (auto& pair1 : pair.second) {
			auto& texture = pair1.second;

			if (!lstrcmpiW(filesystem::path(texture.Path).extension().c_str(), L".dds")) {
				ThrowIfFailed(CreateDDSTextureFromFile(device, resourceUploadBatch, texture.Path.c_str(), texture.Resource.ReleaseAndGetAddressOf()));
			}
			else {
				ThrowIfFailed(CreateWICTextureFromFile(device, resourceUploadBatch, texture.Path.c_str(), texture.Resource.ReleaseAndGetAddressOf()));
			}

			CreateShaderResourceView(device, texture.Resource.Get(), m_resourceDescriptors->GetCpuHandle(texture.DescriptorHeapIndex));
		}
	}

	resourceUploadBatch.End(m_deviceResources->GetCommandQueue()).wait();
}

void D3DApp::BuildRenderItems() {
	if (!m_renderItems.empty()) return;

	const auto AddRenderItem = [&](RenderItem& renderItem, const MaterialBase& material, const PxSphereGeometry& geometry, const PxVec3& position) {
		renderItem.HitGroup = ShaderSubobjects.PrimaryRayHitGroup;

		renderItem.VerticesDescriptorHeapIndex = DescriptorHeapIndices::SphereVertices;
		renderItem.IndicesDescriptorHeapIndex = DescriptorHeapIndices::SphereIndices;

		renderItem.ObjectConstantBufferIndex = m_renderItems.size();

		renderItem.Material = Material(material, renderItem.ObjectConstantBufferIndex);

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
		} spheres[]{
			{
				{ -2, 0.5f, 0 },
				MaterialBase::CreateLambertian({ 0.1f, 0.2f, 0.5f, 1 })
			},
			{
				{ 0, 0.5f, 0 },
				MaterialBase::CreateDielectric({ 1, 1, 1, 1 }, 1.5f)
			},
			{
				{ 2, 0.5f, 0 },
				MaterialBase::CreateMetal({ 0.7f, 0.6f, 0.5f, 1 }, 0.2f)
			}
		};
		for (const auto& sphere : spheres) {
			RenderItem renderItem;
			AddRenderItem(renderItem, sphere.Material, PxSphereGeometry(0.5f), sphere.Position);
		}

		for (int i = -10; i < 10; i++) {
			for (int j = -10; j < 10; j++) {
				constexpr auto A = 0.5f;
				const auto ω = PxTwoPi / m_spring.Period;

				PxVec3 position;
				position.x = static_cast<float>(i) + 0.7f * Random::Float();
				position.y = m_spring.PositionY + SimpleHarmonicMotion::Spring::CalculateDisplacement(A, ω, 0.0f, position.x);
				position.z = static_cast<float>(j) - 0.7f * Random::Float();

				bool isOverlapping = false;
				for (const auto& sphere : spheres) {
					if ((position - sphere.Position).magnitude() < 1) {
						isOverlapping = true;
						break;
					}
				}
				if (isOverlapping) continue;

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

				const auto rigidDynamic = AddRenderItem(renderItem, material, PxSphereGeometry(0.075f), position);

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
				MaterialBase::CreateMetal({ 0.5f, 0.5f, 0.5f, 1 }, 0.8f)
			},
			{
				Objects.Earth,
				m_Earth,
				MaterialBase::CreateLambertian({ 0.1f, 0.2f, 0.5f, 1 })
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

			if (m_textures.contains(object.Name)) renderItem.pTextures = &m_textures[object.Name];

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

void D3DApp::CreateRootSignatures() {
	const auto device = m_deviceResources->GetD3DDevice();

	ThrowIfFailed(device->CreateRootSignature(0, g_pRaytracing, ARRAYSIZE(g_pRaytracing), IID_PPV_ARGS(m_globalRootSignature.ReleaseAndGetAddressOf())));
}

void D3DApp::CreatePipelineStateObjects() {
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

	ThrowIfFailed(device->CreateStateObject(stateObjectDesc, IID_PPV_ARGS(m_pipelineStateObject.ReleaseAndGetAddressOf())));
}

void D3DApp::CreateDescriptorHeaps() {
	const auto device = m_deviceResources->GetD3DDevice();

	m_resourceDescriptors = make_unique<DescriptorHeap>(device, DescriptorHeapIndices::Count);
}

void D3DApp::CreateGeometries() {
	const auto device = m_deviceResources->GetD3DDevice();

	GeometricPrimitive::VertexCollection vertices;
	GeometricPrimitive::IndexCollection indices;

	GeometricPrimitive::CreateGeoSphere(vertices, indices, SphereRadius * 2, 6, false);
	m_sphere = make_unique<decltype(m_sphere)::element_type>(device, vertices, indices, D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE);
	m_sphere->CreateShaderResourceViews(m_resourceDescriptors->GetCpuHandle(DescriptorHeapIndices::SphereVertices), m_resourceDescriptors->GetCpuHandle(DescriptorHeapIndices::SphereIndices));
}

void D3DApp::CreateBottomLevelAccelerationStructure(const vector<D3D12_RAYTRACING_GEOMETRY_DESC>& geometryDescs, AccelerationStructureBuffers& buffers) {
	const auto device = m_deviceResources->GetD3DDevice();

	nv_helpers_dx12::BottomLevelAccelerationStructureGenerator bottomLevelAccelerationStructureGenerator(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE);

	for (const auto& geometryDesc : geometryDescs) bottomLevelAccelerationStructureGenerator.AddGeometry(geometryDesc);

	UINT64 scratchSize, resultSize;
	bottomLevelAccelerationStructureGenerator.ComputeASBufferSizes(device, scratchSize, resultSize);

	buffers = AccelerationStructureBuffers(device, scratchSize, resultSize);

	bottomLevelAccelerationStructureGenerator.Generate(m_deviceResources->GetCommandList(), buffers.Scratch.Get(), buffers.Result.Get());
}

void D3DApp::CreateTopLevelAccelerationStructure(bool updateOnly, AccelerationStructureBuffers& buffers) {
	nv_helpers_dx12::TopLevelAccelerationStructureGenerator topLevelAccelerationStructureGenerator(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE);

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
		topLevelAccelerationStructureGenerator.AddInstance(m_sphereBottomLevelAccelerationStructureBuffers.Result->GetGPUVirtualAddress(), XMLoadFloat4x4(reinterpret_cast<const XMFLOAT4X4*>(world.front())), i, i);
	}

	const auto device = m_deviceResources->GetD3DDevice();

	const auto commandList = m_deviceResources->GetCommandList();

	if (updateOnly) {
		const auto uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(buffers.Result.Get());
		commandList->ResourceBarrier(1, &uavBarrier);
	}
	else {
		UINT64 scratchSize, resultSize, instanceDescsSize;
		topLevelAccelerationStructureGenerator.ComputeASBufferSizes(device, scratchSize, resultSize, instanceDescsSize);
		buffers = AccelerationStructureBuffers(device, scratchSize, resultSize, instanceDescsSize);
	}

	topLevelAccelerationStructureGenerator.Generate(commandList, buffers.Scratch.Get(), buffers.Result.Get(), buffers.InstanceDesc.Get(), updateOnly ? buffers.Result.Get() : nullptr);
}

void D3DApp::CreateAccelerationStructures() {
	const auto commandList = m_deviceResources->GetCommandList();

	commandList->Reset(m_deviceResources->GetCommandAllocator(), nullptr);

	CreateBottomLevelAccelerationStructure({ m_sphere->GetGeometryDesc() }, m_sphereBottomLevelAccelerationStructureBuffers);

	CreateTopLevelAccelerationStructure(false, m_topLevelAccelerationStructureBuffers);

	commandList->Close();

	m_deviceResources->GetCommandQueue()->ExecuteCommandLists(1, CommandListCast(&commandList));

	m_deviceResources->WaitForGpu();
}

void D3DApp::CreateConstantBuffers() {
	m_sceneConstantBuffer = m_graphicsMemory->Allocate(sizeof(SceneConstant));

	const auto renderItemsSize = m_renderItems.size();

	m_objectConstantBuffer = m_graphicsMemory->Allocate(sizeof(ObjectConstant) * renderItemsSize);

	for (size_t i = 0; i < renderItemsSize; i++) {
		const auto& renderItem = m_renderItems[i];

		ObjectConstant& objectConstant = reinterpret_cast<ObjectConstant*>(m_objectConstantBuffer.Memory())[i];

		objectConstant.Material = renderItem.Material;

		if (renderItem.pTextures != nullptr) {
			if (renderItem.pTextures->contains(TextureType::Image)) objectConstant.TextureFlags |= TextureFlags::Image;
			if (renderItem.pTextures->contains(TextureType::Normal)) objectConstant.TextureFlags |= TextureFlags::Normal;
		}
	}
}

void D3DApp::CreateShaderBindingTables() {
	m_shaderBindingTableGenerator = {};

	m_shaderBindingTableGenerator.AddRayGenerationProgram(ShaderEntryPoints.RayGeneration, { nullptr });

	m_shaderBindingTableGenerator.AddMissProgram(ShaderEntryPoints.PrimaryRayMiss, { nullptr });

	for (const auto& renderItem : m_renderItems) {
		D3D12_GPU_VIRTUAL_ADDRESS pImageTexture = NULL, pNormalTexture = NULL;
		if (renderItem.pTextures != nullptr) {
			auto& textures = *renderItem.pTextures;
			if (textures.contains(TextureType::Image)) {
				pImageTexture = m_resourceDescriptors->GetGpuHandle(textures[TextureType::Image].DescriptorHeapIndex).ptr;
			}
			if (textures.contains(TextureType::Normal)) {
				pNormalTexture = m_resourceDescriptors->GetGpuHandle(textures[TextureType::Normal].DescriptorHeapIndex).ptr;
			}
		}

		m_shaderBindingTableGenerator.AddHitGroup(
			renderItem.HitGroup.c_str(),
			{
				reinterpret_cast<void*>(m_resourceDescriptors->GetGpuHandle(renderItem.VerticesDescriptorHeapIndex).ptr),
				reinterpret_cast<void*>(m_resourceDescriptors->GetGpuHandle(renderItem.IndicesDescriptorHeapIndex).ptr),
				reinterpret_cast<void*>(pImageTexture),
				reinterpret_cast<void*>(pNormalTexture),
				reinterpret_cast<ObjectConstant*>(m_objectConstantBuffer.GpuAddress()) + renderItem.ObjectConstantBufferIndex
			}
		);
	}

	const auto device = m_deviceResources->GetD3DDevice();

	ThrowIfFailed(CreateUploadBuffer(device, m_shaderBindingTableGenerator.ComputeSBTSize(), m_shaderBindingTable.ReleaseAndGetAddressOf()));

	Microsoft::WRL::ComPtr<ID3D12StateObjectProperties> stateObjectProperties;
	ThrowIfFailed(m_pipelineStateObject->QueryInterface(IID_PPV_ARGS(&stateObjectProperties)));

	m_shaderBindingTableGenerator.Generate(m_shaderBindingTable.Get(), stateObjectProperties.Get());
}

void D3DApp::ProcessInput() {
	GamePad::State gamepadStates[GamePad::MAX_PLAYER_COUNT];
	for (int i = 0; i < GamePad::MAX_PLAYER_COUNT; i++) {
		gamepadStates[i] = m_gamepad->GetState(i);
		m_gamepadButtonStateTrackers[i].Update(gamepadStates[i]);
	}

	const auto keyboardState = m_keyboard->GetState();
	m_keyboardStateTracker.Update(keyboardState);

	const auto mouseState = m_mouse->GetState();
	m_mouseButtonStateTracker.Update(mouseState);

	UpdateCamera(gamepadStates, keyboardState, mouseState);

	{
		for (const auto& gamepadButtonStateTracker : m_gamepadButtonStateTrackers) {
			if (gamepadButtonStateTracker.menu == GamepadButtonState::PRESSED) m_isRunning = !m_isRunning;
			if (gamepadButtonStateTracker.x == GamepadButtonState::PRESSED) m_Earth.IsGravityEnabled = !m_Earth.IsGravityEnabled;
			if (gamepadButtonStateTracker.b == GamepadButtonState::PRESSED) m_star.IsGravityEnabled = !m_star.IsGravityEnabled;
		}
	}

	{
		if (m_keyboardStateTracker.IsKeyPressed(Key::Escape)) m_isRunning = !m_isRunning;
		if (m_keyboardStateTracker.IsKeyPressed(Key::G)) m_Earth.IsGravityEnabled = !m_Earth.IsGravityEnabled;
		if (m_keyboardStateTracker.IsKeyPressed(Key::H)) m_star.IsGravityEnabled = !m_star.IsGravityEnabled;
	}
}

void D3DApp::UpdateCamera(const GamePad::State(&gamepadStates)[8], const Keyboard::State& keyboardState, const Mouse::State& mouseState) {
	constexpr auto XMFLOAT3_to_PxVec3 = [](const XMFLOAT3& value) { return PxVec3(value.x, value.y, value.z); };

	const auto rayOrigin = XMFLOAT3_to_PxVec3(m_camera.GetPosition());

	PxScene* scene;
	m_myPhysX.GetPhysics().getScenes(&scene, sizeof(scene));

	const auto Translate = [&](const XMFLOAT3& displacement) {
		if (!displacement.x && !displacement.y && !displacement.z) return;

		auto x = XMFLOAT3_to_PxVec3(m_camera.GetRightDirection() * displacement.x + m_camera.GetUpDirection() * displacement.y + m_camera.GetForwardDirection() * displacement.z);
		const auto magnitude = x.magnitude();
		const auto normalized = x / magnitude;
		PxRaycastBuffer raycastBuffer;
		if (scene->raycast(rayOrigin, normalized, FLT_MAX, raycastBuffer) && raycastBuffer.block.distance < magnitude) {
			x = normalized * max(raycastBuffer.block.distance - 0.1f, 0.0f);
		}

		m_camera.Translate({ x.x, x.y, x.z });
	};

	const auto elapsedSeconds = static_cast<float>(m_stepTimer.GetElapsedSeconds());

	{
		const auto rotationSpeed = -XM_2PI * 0.4f * elapsedSeconds;
		for (const auto& gamepadState : gamepadStates) {
			if (gamepadState.IsConnected()) {
				const auto translationSpeed = (gamepadState.IsLeftStickPressed() ? 30.0f : 15.0f) * elapsedSeconds;
				const XMFLOAT3 displacement{ gamepadState.thumbSticks.leftX * translationSpeed, 0, gamepadState.thumbSticks.leftY * translationSpeed };
				Translate(displacement);

				m_camera.Yaw(gamepadState.thumbSticks.rightX * rotationSpeed);
				m_camera.Pitch(gamepadState.thumbSticks.rightY * rotationSpeed);
			}
		}
	}

	{
		if (mouseState.positionMode == Mouse::MODE_RELATIVE) {
			const auto translationSpeed = (keyboardState.LeftShift ? 30.0f : 15.0f) * elapsedSeconds;
			XMFLOAT3 displacement{};
			if (keyboardState.A) displacement.x -= translationSpeed;
			if (keyboardState.D) displacement.x += translationSpeed;
			if (keyboardState.W) displacement.z += translationSpeed;
			if (keyboardState.S) displacement.z -= translationSpeed;
			Translate(displacement);

			const auto rotationSpeed = -20 * elapsedSeconds;
			m_camera.Yaw(XMConvertToRadians(static_cast<float>(mouseState.x)) * rotationSpeed);
			m_camera.Pitch(XMConvertToRadians(static_cast<float>(mouseState.y)) * rotationSpeed);
		}
	}

	m_camera.UpdateView();
}

void D3DApp::UpdateSceneConstantBuffer() {
	auto& sceneConstant = *reinterpret_cast<SceneConstant*>(m_sceneConstantBuffer.Memory());

	XMStoreFloat4x4(&sceneConstant.ProjectionToWorld, XMMatrixTranspose(XMMatrixInverse(nullptr, m_camera.GetView() * m_camera.GetProjection())));

	sceneConstant.CameraPosition = m_camera.GetPosition();

	sceneConstant.AntiAliasingSampleCount = m_antiAliasingSampleCount;

	sceneConstant.FrameCount = m_stepTimer.GetFrameCount();
}

void D3DApp::UpdateRenderItem(RenderItem& renderItem) const {
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

void D3DApp::DispatchRays() {
	const auto commandList = m_deviceResources->GetCommandList();

	if (m_isRunning) CreateTopLevelAccelerationStructure(true, m_topLevelAccelerationStructureBuffers);

	ID3D12DescriptorHeap* const descriptorHeaps[]{ m_resourceDescriptors->Heap() };
	commandList->SetDescriptorHeaps(ARRAYSIZE(descriptorHeaps), descriptorHeaps);

	commandList->SetComputeRootSignature(m_globalRootSignature.Get());
	commandList->SetComputeRootDescriptorTable(0, m_resourceDescriptors->GetGpuHandle(DescriptorHeapIndices::Output));
	commandList->SetComputeRootShaderResourceView(1, m_topLevelAccelerationStructureBuffers.Result->GetGPUVirtualAddress());
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
