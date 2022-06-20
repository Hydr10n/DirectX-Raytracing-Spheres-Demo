#include "pch.h"

#include "D3DApp.h"

#include "SharedData.h"
#include "MyAppData.h"

#include "DeviceResources.h"

#include "StepTimer.h"

#include "GraphicsMemoryEx.h"

#include "DirectXTK12/GamePad.h"
#include "DirectXTK12/Keyboard.h"
#include "DirectXTK12/Mouse.h"

#include "DirectXTK12/GeometricPrimitive.h"

#include "BottomLevelAccelerationStructureGenerator.h"
#include "TopLevelAccelerationStructureGenerator.h"
#include "ShaderBindingTableGenerator.h"

#include "DirectXRaytracingHelpers.h"

#include "TemporalAntiAliasingEffect.h"

#include "Material.h"
#include "Texture.h"

#include "Camera.h"

#include "MyPhysX.h"

#include "Shaders/Raytracing.hlsl.h"

#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"
#include "ImGuiEx.h"

#include "Random.h"

#include <shellapi.h>

using namespace DirectX;
using namespace DirectX::RaytracingHelpers;
using namespace DisplayHelpers;
using namespace DX;
using namespace Microsoft::WRL;
using namespace nv_helpers_dx12;
using namespace PhysicsHelpers;
using namespace physx;
using namespace std;
using namespace WindowHelpers;

using GraphicsSettingsData = MyAppData::Settings::Graphics;

struct D3DApp::Impl : IDeviceNotify {
	Impl(const shared_ptr<WindowModeHelper>& windowModeHelper) noexcept(false) : m_windowModeHelper(windowModeHelper) {
		{
			if (GraphicsSettingsData::Load<GraphicsSettingsData::Raytracing::SamplesPerPixel>(m_raytracingSamplesPerPixel)) {
				m_raytracingSamplesPerPixel = clamp(m_raytracingSamplesPerPixel, 1u, MaxRaytracingSamplesPerPixel);
			}

			{
				GraphicsSettingsData::Load<GraphicsSettingsData::TemporalAntiAliasing::IsEnabled>(m_isTemporalAntiAliasingEnabled);

				if (GraphicsSettingsData::Load<GraphicsSettingsData::TemporalAntiAliasing::Alpha>(m_temporalAntiAliasingConstants.Alpha)) {
					m_temporalAntiAliasingConstants.Alpha = clamp(m_temporalAntiAliasingConstants.Alpha, 0.0f, 1.0f);
				}

				if (GraphicsSettingsData::Load<GraphicsSettingsData::TemporalAntiAliasing::ColorBoxSigma>(m_temporalAntiAliasingConstants.ColorBoxSigma)) {
					m_temporalAntiAliasingConstants.ColorBoxSigma = clamp(m_temporalAntiAliasingConstants.ColorBoxSigma, 0.0f, MaxTemporalAntiAliasingColorBoxSigma);
				}
			}
		}

		{
			ImGui::CreateContext();

			ImGui::StyleColorsDark();

			auto& IO = ImGui::GetIO();

			IO.IniFilename = IO.LogFilename = nullptr;

			IO.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
			IO.BackendFlags |= ImGuiBackendFlags_HasGamepad;

			ImGui_ImplWin32_Init(m_windowModeHelper->hWnd);
		}

		BuildTextures();

		BuildRenderItems();

		m_deviceResources->RegisterDeviceNotify(this);

		m_deviceResources->SetWindow(windowModeHelper->hWnd, windowModeHelper->Resolution);

		m_deviceResources->CreateDeviceResources();
		CreateDeviceDependentResources();

		m_deviceResources->CreateWindowSizeDependentResources();
		CreateWindowSizeDependentResources();

		m_inputDevices.Mouse->SetWindow(windowModeHelper->hWnd);

		m_firstPersonCamera.SetPosition({ 0, 0, -15 });
	}

	~Impl() {
		m_deviceResources->WaitForGpu();

		{
			if (ImGui::GetIO().BackendRendererUserData != nullptr) ImGui_ImplDX12_Shutdown();

			ImGui_ImplWin32_Shutdown();

			ImGui::DestroyContext();
		}
	}

	SIZE GetOutputSize() const noexcept { return m_deviceResources->GetOutputSize(); }

	void Tick() {
		m_stepTimer.Tick([&] { Update(); });

		const auto& windowModeHelper = m_windowModeHelper;

		const auto windowMode = windowModeHelper->GetMode();
		const auto resolution = windowModeHelper->Resolution;
		const auto windowedStyle = windowModeHelper->WindowedStyle, windowedExStyle = windowModeHelper->WindowedExStyle;

		Render();

		const auto newWindowMode = windowModeHelper->GetMode();
		const auto newResolution = windowModeHelper->Resolution;
		const auto isWindowModeChanged = newWindowMode != windowMode, isResolutionChanged = newResolution != resolution;
		if (isWindowModeChanged || isResolutionChanged || windowModeHelper->WindowedStyle != windowedStyle || windowModeHelper->WindowedExStyle != windowedExStyle) {
			ThrowIfFailed(windowModeHelper->Apply());

			if (isWindowModeChanged) GraphicsSettingsData::Save<GraphicsSettingsData::WindowMode>(newWindowMode);
			if (isResolutionChanged) GraphicsSettingsData::Save<GraphicsSettingsData::Resolution>(newResolution);
		}
	}

	void OnWindowSizeChanged() {
		if (m_deviceResources->WindowSizeChanged(m_windowModeHelper->Resolution)) {
			CreateWindowSizeDependentResources();
		}
	}

	void OnDisplayChanged() { m_deviceResources->UpdateColorSpace(); }

	void OnResuming() {
		m_stepTimer.ResetElapsedTime();

		m_inputDevices.Gamepad->Resume();
	}

	void OnSuspending() { m_inputDevices.Gamepad->Suspend(); }

	void OnActivated() {}

	void OnDeactivated() {}

	void OnDeviceLost() override {
		ImGui_ImplDX12_Shutdown();

		m_renderTextureResources = {};

		for (auto& pair : m_textures) for (auto& pair1 : get<0>(pair.second)) pair1.second.Resource.Reset();

		m_shaderBindingTable.Reset();

		m_accelerationStructureBuffers = {};

		m_geometries = {};

		m_constantBuffers = {};

		m_temporalAntiAliasingEffect.reset();

		m_pipelineStateObject.Reset();

		m_globalRootSignature.Reset();

		m_resourceDescriptors.reset();

		m_graphicsMemory.reset();
	}

	void OnDeviceRestored() override {
		CreateDeviceDependentResources();

		CreateWindowSizeDependentResources();
	}

private:
	using GamepadButtonState = GamePad::ButtonStateTracker::ButtonState;
	using Key = Keyboard::Keys;

	struct ShaderEntryPoints {
		static constexpr LPCWSTR
			RayGeneration = L"RayGeneration",
			RadianceRayMiss = L"RadianceRayMiss", RadianceRayClosestHit = L"RadianceRayClosestHit";
	};

	struct ShaderSubobjects { static constexpr LPCWSTR RadianceRayHitGroup = L"RadianceRayHitGroup"; };

	struct Objects {
		static constexpr LPCSTR
			Environment = "Environment",
			Earth = "Earth", Moon = "Moon", Star = "Star",
			HarmonicOscillator = "HarmonicOscillator";
	};

	const shared_ptr<WindowModeHelper> m_windowModeHelper;

	unique_ptr<DeviceResources> m_deviceResources = make_unique<decltype(m_deviceResources)::element_type>();

	StepTimer m_stepTimer;

	unique_ptr<GraphicsMemoryEx> m_graphicsMemory;

	const struct {
		unique_ptr<GamePad> Gamepad = make_unique<decltype(Gamepad)::element_type>();
		unique_ptr<Keyboard> Keyboard = make_unique<decltype(Keyboard)::element_type>();
		unique_ptr<Mouse> Mouse = make_unique<decltype(Mouse)::element_type>();
	} m_inputDevices;

	struct {
		GamePad::ButtonStateTracker Gamepad;
		Keyboard::KeyboardStateTracker Keyboard;
		Mouse::ButtonStateTracker Mouse;
	} m_inputDeviceStateTrackers;

	struct DescriptorHeapIndex {
		enum {
			PreviousOutputSRV, CurrentOutputSRV, CurrentOutputUAV, MotionVectorsSRV, MotionVectorsUAV, FinalOutputUAV,
			SphereVertices, SphereIndices,
			EnvironmentCubeMap,
			MoonColorMap, MoonNormalMap,
			EarthColorMap, EarthNormalMap,
			Font,
			Count
		};
	};
	unique_ptr<DescriptorHeap> m_resourceDescriptors;

	ComPtr<ID3D12RootSignature> m_globalRootSignature;

	ComPtr<ID3D12StateObject> m_pipelineStateObject;

	static constexpr float MaxTemporalAntiAliasingColorBoxSigma = 2;
	bool m_isTemporalAntiAliasingEnabled = true;
	decltype(TemporalAntiAliasingEffect::Constants) m_temporalAntiAliasingConstants;
	unique_ptr<TemporalAntiAliasingEffect> m_temporalAntiAliasingEffect;

	struct SceneConstants {
		UINT RaytracingSamplesPerPixel, FrameCount;
		BOOL IsEnvironmentCubeMapUsed;
		float _padding;
		XMMATRIX EnvironmentMapTransform;
		Camera Camera;
	};
	struct TextureFlags { enum { ColorMap = 0x1, NormalMap = 0x2 }; };
	struct ObjectConstants {
		UINT TextureFlags;
		XMFLOAT3 _padding;
		XMMATRIX TextureTransform;
		Material Material;
	};
	struct {
		GraphicsResourceEx<SceneConstants> Scene;
		GraphicsResourceArray<ObjectConstants> Objects;
	} m_constantBuffers;

	struct {
		struct Sphere : unique_ptr<Triangles<VertexPositionNormalTexture, UINT16>> {
			using unique_ptr::unique_ptr;
			using unique_ptr::operator=;
			static constexpr float Radius = 0.5f;
		} Sphere;
	} m_geometries;

	struct {
		struct { AccelerationStructureBuffers Sphere; } BottomLevel;
		AccelerationStructureBuffers TopLevel;
	} m_accelerationStructureBuffers;

	ShaderBindingTableGenerator m_shaderBindingTableGenerator;
	ComPtr<ID3D12Resource> m_shaderBindingTable;

	struct { ComPtr<ID3D12Resource> PreviousOutput, CurrentOutput, MotionVectors, FinalOutput; } m_renderTextureResources;

	TextureDictionary m_textures;

	FirstPersonCamera m_firstPersonCamera;

	struct RenderItem {
		string Name;
		wstring HitGroup;
		struct { UINT Vertices = UINT_MAX, Indices = UINT_MAX; } DescriptorHeapIndices;
		struct { UINT Object = UINT_MAX; } ConstantBufferIndices;
		Material Material;
		TextureDictionary::mapped_type* pTextures{};
		PxShape* Shape{};
	};
	vector<RenderItem> m_renderItems;

	bool m_isPhysicsSimulationRunning = true;
	struct {
		map<string, tuple<PxRigidBody*, bool /*IsGravityEnabled*/>> RigidBodies;
		const struct { PxReal PositionY = 0.5f, Period = 3; } Spring;
	} m_physicsObjects;
	MyPhysX m_myPhysX;

	static constexpr UINT MaxRaytracingSamplesPerPixel = 16;
	UINT m_raytracingSamplesPerPixel = 2;

	bool m_isMenuOpen{};

	void CreateDeviceDependentResources() {
		const auto device = m_deviceResources->GetD3DDevice();

		m_graphicsMemory = make_unique<decltype(m_graphicsMemory)::element_type>(device);

		CreateDescriptorHeaps();

		CreateRootSignatures();

		CreatePipelineStateObjects();

		CreateEffects();

		CreateConstantBuffers();

		CreateGeometries();

		CreateAccelerationStructures();

		CreateShaderBindingTables();

		LoadTextures();
	}

	void CreateWindowSizeDependentResources() {
		const auto device = m_deviceResources->GetD3DDevice();

		const auto outputSize = GetOutputSize();

		{
			const auto CreateSRVDesc = [](DXGI_FORMAT format) {
				return D3D12_SHADER_RESOURCE_VIEW_DESC{
					.Format = format,
					.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
					.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
					.Texture2D = {
						.MipLevels = 1
					}
				};
			};

			const auto CreateResource = [&](const D3D12_RESOURCE_DESC& resourceDesc, ComPtr<ID3D12Resource>& resource, const D3D12_SHADER_RESOURCE_VIEW_DESC* pSrvDesc = nullptr, UINT srvDescriptorHeapIndex = UINT_MAX, UINT uavDescriptorHeapIndex = UINT_MAX) {
				const CD3DX12_HEAP_PROPERTIES defaultHeapProperties(D3D12_HEAP_TYPE_DEFAULT);
				const D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{ .ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D };

				ThrowIfFailed(device->CreateCommittedResource(&defaultHeapProperties, D3D12_HEAP_FLAG_NONE, &resourceDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(resource.ReleaseAndGetAddressOf())));

				if (pSrvDesc != nullptr && srvDescriptorHeapIndex != UINT_MAX) {
					device->CreateShaderResourceView(resource.Get(), pSrvDesc, m_resourceDescriptors->GetCpuHandle(srvDescriptorHeapIndex));
				}

				if (uavDescriptorHeapIndex != UINT_MAX) {
					device->CreateUnorderedAccessView(resource.Get(), nullptr, &uavDesc, m_resourceDescriptors->GetCpuHandle(uavDescriptorHeapIndex));
				}
			};

			{
				const auto tex2DDesc = CD3DX12_RESOURCE_DESC::Tex2D(m_deviceResources->GetBackBufferFormat(), outputSize.cx, outputSize.cy, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
				const auto srvDesc = CreateSRVDesc(tex2DDesc.Format);

				CreateResource(tex2DDesc, m_renderTextureResources.PreviousOutput, &srvDesc, DescriptorHeapIndex::PreviousOutputSRV);
				CreateResource(tex2DDesc, m_renderTextureResources.CurrentOutput, &srvDesc, DescriptorHeapIndex::CurrentOutputSRV, DescriptorHeapIndex::CurrentOutputUAV);
				CreateResource(tex2DDesc, m_renderTextureResources.FinalOutput, nullptr, UINT_MAX, DescriptorHeapIndex::FinalOutputUAV);
			}

			{
				const auto tex2DDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32G32_FLOAT, outputSize.cx, outputSize.cy, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
				const auto srvDesc = CreateSRVDesc(tex2DDesc.Format);

				CreateResource(tex2DDesc, m_renderTextureResources.MotionVectors, &srvDesc, DescriptorHeapIndex::MotionVectorsSRV, DescriptorHeapIndex::MotionVectorsUAV);
			}

			m_temporalAntiAliasingEffect->Textures.Size = outputSize;
		}

		m_firstPersonCamera.SetLens(XM_PIDIV4, static_cast<float>(outputSize.cx) / static_cast<float>(outputSize.cy), 1e-1f, 1e4f);

		{
			auto& IO = ImGui::GetIO();

			if (IO.BackendRendererUserData != nullptr) ImGui_ImplDX12_Shutdown();
			ImGui_ImplDX12_Init(device, static_cast<int>(m_deviceResources->GetBackBufferCount()), m_deviceResources->GetBackBufferFormat(), m_resourceDescriptors->Heap(), m_resourceDescriptors->GetCpuHandle(DescriptorHeapIndex::Font), m_resourceDescriptors->GetGpuHandle(DescriptorHeapIndex::Font));

			IO.Fonts->Clear();
			IO.Fonts->AddFontFromFileTTF(R"(C:\Windows\Fonts\segoeui.ttf)", static_cast<float>(outputSize.cy) * 0.025f);
		}
	}

	void Update() {
		PIXBeginEvent(PIX_COLOR_DEFAULT, L"Update");

		ProcessInputs();

		UpdateSceneConstantBuffer();

		if (m_isPhysicsSimulationRunning) m_myPhysX.Tick(static_cast<PxReal>(min(1.0 / 60, m_stepTimer.GetElapsedSeconds())));

		PIXEndEvent();
	}

	void Clear() {
		const auto commandList = m_deviceResources->GetCommandList();

		PIXBeginEvent(commandList, PIX_COLOR_DEFAULT, L"Clear");

		const auto renderTargetView = m_deviceResources->GetRenderTargetView(), depthStencilView = m_deviceResources->GetDepthStencilView();
		commandList->OMSetRenderTargets(1, &renderTargetView, FALSE, &depthStencilView);
		commandList->ClearRenderTargetView(renderTargetView, Colors::Black, 0, nullptr);
		commandList->ClearDepthStencilView(depthStencilView, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1, 0, 0, nullptr);

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

		ID3D12DescriptorHeap* const descriptorHeaps[]{ m_resourceDescriptors->Heap() };
		commandList->SetDescriptorHeaps(ARRAYSIZE(descriptorHeaps), descriptorHeaps);

		{
			DispatchRays();

			if (m_isTemporalAntiAliasingEnabled) {
				const ScopedBarrier scopedBarrier(
					commandList,
					{
						CD3DX12_RESOURCE_BARRIER::Transition(m_renderTextureResources.PreviousOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
						CD3DX12_RESOURCE_BARRIER::Transition(m_renderTextureResources.CurrentOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
						CD3DX12_RESOURCE_BARRIER::Transition(m_renderTextureResources.MotionVectors.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
					}
				);

				m_temporalAntiAliasingEffect->Apply(commandList);
			}

			{
				const auto
					renderTarget = m_deviceResources->GetRenderTarget(),
					previousOutput = m_renderTextureResources.PreviousOutput.Get(),
					output = (m_isTemporalAntiAliasingEnabled ? m_renderTextureResources.FinalOutput : m_renderTextureResources.CurrentOutput).Get();

				const ScopedBarrier scopedBarrier(
					commandList,
					{
						CD3DX12_RESOURCE_BARRIER::Transition(renderTarget, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST),
						CD3DX12_RESOURCE_BARRIER::Transition(previousOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST),
						CD3DX12_RESOURCE_BARRIER::Transition(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE)
					}
				);

				commandList->CopyResource(renderTarget, output);
				commandList->CopyResource(previousOutput, output);
			}
		}

		if (m_isMenuOpen) RenderMenu();

		PIXEndEvent(commandList);

		PIXBeginEvent(PIX_COLOR_DEFAULT, L"Present");

		m_deviceResources->Present();

		m_graphicsMemory->Commit(m_deviceResources->GetCommandQueue());

		m_deviceResources->WaitForGpu();

		PIXEndEvent();
	}

	void BuildTextures() {
		m_textures = {
			{
				Objects::Environment,
				{
					{
						{
							TextureType::CubeMap,
							Texture{
								.DescriptorHeapIndex = DescriptorHeapIndex::EnvironmentCubeMap,
								.Path = L"Space.dds"
							}
						}
					},
					XMMatrixRotationRollPitchYaw(XM_PI, XM_PI * 0.2f, 0)
				}
			},
			{
				Objects::Moon,
				{
					{
						{
							TextureType::ColorMap,
							Texture{
								.DescriptorHeapIndex = DescriptorHeapIndex::MoonColorMap,
								.Path = L"Moon.jpg"
							}
						},
						{
							TextureType::NormalMap,
							Texture{
								.DescriptorHeapIndex = DescriptorHeapIndex::MoonNormalMap,
								.Path = L"Moon_Normal.jpg"
							}
						}
					},
					XMMatrixTranslation(0.5f, 0, 0)
				}
			},
			{
				Objects::Earth,
				{
					{
						{
							TextureType::ColorMap,
							Texture{
								.DescriptorHeapIndex = DescriptorHeapIndex::EarthColorMap,
								.Path = L"Earth.jpg"
							}
						},
						{
							TextureType::NormalMap,
							Texture{
								.DescriptorHeapIndex = DescriptorHeapIndex::EarthNormalMap,
								.Path = L"Earth_Normal.jpg"
							}
						}
					},
					XMMatrixIdentity()
				}
			}
		};

		m_textures.DirectoryPath = filesystem::path(*__wargv).replace_filename(LR"(Textures\)");
	}

	void LoadTextures() {
		const auto device = m_deviceResources->GetD3DDevice();
		const auto commandQueue = m_deviceResources->GetCommandQueue();
		constexpr auto threadCount = 8;

		m_textures.Load(device, commandQueue, *m_resourceDescriptors, threadCount);
	}

	void BuildRenderItems() {
		m_renderItems.clear();

		const auto& material = *m_myPhysX.GetPhysics().createMaterial(0.5f, 0.5f, 0.6f);

		const auto AddRenderItem = [&](RenderItem& renderItem, const PxSphereGeometry& geometry, const PxVec3& position) -> auto& {
			renderItem.HitGroup = ShaderSubobjects::RadianceRayHitGroup;

			renderItem.DescriptorHeapIndices = {
				.Vertices = DescriptorHeapIndex::SphereVertices,
				.Indices = DescriptorHeapIndex::SphereIndices
			};

			renderItem.ConstantBufferIndices = { .Object = static_cast<UINT>(m_renderItems.size()) };

			auto& rigidDynamic = *m_myPhysX.GetPhysics().createRigidDynamic(PxTransform(position));

			m_myPhysX.GetScene().addActor(rigidDynamic);

			renderItem.Shape = PxRigidActorExt::createExclusiveShape(rigidDynamic, geometry, material);

			PxRigidBodyExt::updateMassAndInertia(rigidDynamic, 1);

			rigidDynamic.setAngularDamping(0);

			m_renderItems.emplace_back(renderItem);

			return rigidDynamic;
		};

		{
			const struct {
				PxVec3 Position;
				Material Material;
			} spheres[]{
				{
					{ -2, 0.5f, 0 },
					Material().AsLambertian({ 0.1f, 0.2f, 0.5f, 1 })
				},
				{
					{ 0, 0.5f, 0 },
					Material().AsDielectric({ 1, 1, 1, 1 }, 1.5f)
				},
				{
					{ 2, 0.5f, 0 },
					Material().AsMetal({ 0.7f, 0.6f, 0.5f, 1 }, 0.2f)
				}
			};
			for (const auto& sphere : spheres) {
				RenderItem renderItem;
				renderItem.Material = sphere.Material;
				AddRenderItem(renderItem, PxSphereGeometry(0.5f), sphere.Position);
			}

			Random random;
			for (int i = -10; i < 10; i++) {
				for (int j = -10; j < 10; j++) {
					constexpr auto A = 0.5f;
					const auto ω = PxTwoPi / m_physicsObjects.Spring.Period;

					PxVec3 position;
					position.x = static_cast<float>(i) + 0.7f * random.Float();
					position.y = m_physicsObjects.Spring.PositionY + SimpleHarmonicMotion::Spring::CalculateDisplacement(A, ω, 0.0f, position.x);
					position.z = static_cast<float>(j) - 0.7f * random.Float();

					bool isOverlapping = false;
					for (const auto& sphere : spheres) {
						if ((position - sphere.Position).magnitude() < 1) {
							isOverlapping = true;
							break;
						}
					}
					if (isOverlapping) continue;

					RenderItem renderItem;

					renderItem.Name = Objects::HarmonicOscillator;

					const auto randomValue = random.Float();
					if (randomValue < 0.5f) renderItem.Material.AsLambertian(random.Float4());
					else if (randomValue < 0.75f) renderItem.Material.AsMetal(random.Float4(0.5f), random.Float(0, 0.5f));
					else renderItem.Material.AsDielectric(random.Float4(), 1.5f);

					auto& rigidDynamic = AddRenderItem(renderItem, PxSphereGeometry(0.075f), position);
					rigidDynamic.setLinearVelocity({ 0, SimpleHarmonicMotion::Spring::CalculateVelocity(A, ω, 0.0f, position.x), 0 });
				}
			}
		}

		{
			const struct {
				LPCSTR Name;
				PxVec3 Position;
				PxReal Radius, RotationPeriod, OrbitalPeriod, Mass;
				Material Material;
			} moon{
				.Name = Objects::Moon,
				.Position = { -4, 4, 0 },
				.Radius = 0.25f,
				.OrbitalPeriod = 10,
				.Material = Material().AsMetal({ 0.5f, 0.5f, 0.5f, 1 }, 0.8f)
			}, earth{
				.Name = Objects::Earth,
				.Position = { 0, moon.Position.y, 0 },
				.Radius = 1,
				.RotationPeriod = 15,
				.Mass = UniversalGravitation::CalculateMass((moon.Position - earth.Position).magnitude(), moon.OrbitalPeriod),
				.Material = Material().AsLambertian({ 0.1f, 0.2f, 0.5f, 1 })
			}, star{
				.Name = Objects::Star,
				.Position = { 0, -50.1f, 0 },
				.Radius = 50,
				.Material = Material().AsMetal({ 0.5f, 0.5f, 0.5f, 1 }, 0)
			};
			for (const auto& sphere : { moon, earth, star }) {
				RenderItem renderItem;

				renderItem.Name = sphere.Name;

				renderItem.Material = sphere.Material;

				if (m_textures.contains(sphere.Name)) renderItem.pTextures = &m_textures[sphere.Name];

				auto& rigidDynamic = AddRenderItem(renderItem, PxSphereGeometry(sphere.Radius), sphere.Position);
				if (renderItem.Name == Objects::Moon) {
					const auto x = earth.Position - sphere.Position;
					const auto magnitude = x.magnitude();
					const auto normalized = x / magnitude;
					const auto linearSpeed = UniversalGravitation::CalculateFirstCosmicSpeed(earth.Mass, magnitude);
					rigidDynamic.setLinearVelocity(linearSpeed * PxVec3(-normalized.z, 0, normalized.x));
					rigidDynamic.setAngularVelocity({ 0, linearSpeed / magnitude, 0 });
				}
				else if (renderItem.Name == Objects::Earth) {
					rigidDynamic.setAngularVelocity({ 0, PxTwoPi / sphere.RotationPeriod, 0 });
					PxRigidBodyExt::setMassAndUpdateInertia(rigidDynamic, &sphere.Mass, 1);
				}
				else if (renderItem.Name == Objects::Star) rigidDynamic.setMass(0);

				m_physicsObjects.RigidBodies[sphere.Name] = { &rigidDynamic, false };
			}
		}
	}

	void CreateDescriptorHeaps() {
		const auto device = m_deviceResources->GetD3DDevice();

		m_resourceDescriptors = make_unique<DescriptorHeap>(device, DescriptorHeapIndex::Count);
	}

	void CreateRootSignatures() {
		const auto device = m_deviceResources->GetD3DDevice();

		ThrowIfFailed(device->CreateRootSignature(0, g_pRaytracing, ARRAYSIZE(g_pRaytracing), IID_PPV_ARGS(m_globalRootSignature.ReleaseAndGetAddressOf())));
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

		dxilLibrary->DefineExport(ShaderEntryPoints::RayGeneration);

		dxilLibrary->DefineExport(ShaderEntryPoints::RadianceRayMiss);
		dxilLibrary->DefineExport(ShaderEntryPoints::RadianceRayClosestHit);
		dxilLibrary->DefineExport(ShaderSubobjects::RadianceRayHitGroup);
		dxilLibrary->DefineExport(L"RadianceRayLocalRootSignatureAssociation");

		ThrowIfFailed(device->CreateStateObject(stateObjectDesc, IID_PPV_ARGS(m_pipelineStateObject.ReleaseAndGetAddressOf())));
	}

	void CreateEffects() {
		const auto device = m_deviceResources->GetD3DDevice();

		m_temporalAntiAliasingEffect = make_unique<decltype(m_temporalAntiAliasingEffect)::element_type>(device);
		m_temporalAntiAliasingEffect->Constants = m_temporalAntiAliasingConstants;
		m_temporalAntiAliasingEffect->Textures = {
			.PreviousOutputSRV = m_resourceDescriptors->GetGpuHandle(DescriptorHeapIndex::PreviousOutputSRV),
			.CurrentOutputSRV = m_resourceDescriptors->GetGpuHandle(DescriptorHeapIndex::CurrentOutputSRV),
			.MotionVectorsSRV = m_resourceDescriptors->GetGpuHandle(DescriptorHeapIndex::MotionVectorsSRV),
			.FinalOutputUAV = m_resourceDescriptors->GetGpuHandle(DescriptorHeapIndex::FinalOutputUAV)
		};
	}

	void CreateConstantBuffers() {
		{
			m_constantBuffers.Scene = m_graphicsMemory->AllocateConstant<SceneConstants>();

			auto& sceneConstants = *m_constantBuffers.Scene.Memory();

			sceneConstants.RaytracingSamplesPerPixel = m_raytracingSamplesPerPixel;

			if (m_textures.contains(Objects::Environment)) {
				const auto& textures = m_textures[Objects::Environment];
				if (get<0>(textures).contains(TextureType::CubeMap)) sceneConstants.IsEnvironmentCubeMapUsed = TRUE;

				sceneConstants.EnvironmentMapTransform = XMMatrixTranspose(get<1>(textures));
			}
		}

		{
			m_constantBuffers.Objects = m_graphicsMemory->AllocateConstants<ObjectConstants>(m_renderItems.size());

			for (const auto& renderItem : m_renderItems) {
				auto& objectConstants = *m_constantBuffers.Objects.Memory(renderItem.ConstantBufferIndices.Object);

				objectConstants.Material = renderItem.Material;

				if (renderItem.pTextures != nullptr) {
					const auto& textures = get<0>(*renderItem.pTextures);
					if (textures.contains(TextureType::ColorMap)) objectConstants.TextureFlags |= TextureFlags::ColorMap;
					if (textures.contains(TextureType::NormalMap)) objectConstants.TextureFlags |= TextureFlags::NormalMap;

					objectConstants.TextureTransform = XMMatrixTranspose(get<1>(*renderItem.pTextures));
				}
			}
		}
	}

	void CreateGeometries() {
		const auto device = m_deviceResources->GetD3DDevice();

		{
			auto& sphere = m_geometries.Sphere;

			GeometricPrimitive::VertexCollection vertices;
			GeometricPrimitive::IndexCollection indices;
			GeometricPrimitive::CreateGeoSphere(vertices, indices, sphere.Radius * 2, 6);

			sphere = make_unique<decay_t<decltype(sphere)>::element_type>(device, vertices, indices, D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE);
			sphere->CreateShaderResourceViews(m_resourceDescriptors->GetCpuHandle(DescriptorHeapIndex::SphereVertices), m_resourceDescriptors->GetCpuHandle(DescriptorHeapIndex::SphereIndices));
		}
	}

	void CreateBottomLevelAccelerationStructure(const vector<D3D12_RAYTRACING_GEOMETRY_DESC>& geometryDescs, AccelerationStructureBuffers& buffers) {
		const auto device = m_deviceResources->GetD3DDevice();

		BottomLevelAccelerationStructureGenerator bottomLevelAccelerationStructureGenerator(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE);

		for (const auto& geometryDesc : geometryDescs) bottomLevelAccelerationStructureGenerator.AddGeometry(geometryDesc);

		UINT64 scratchSize, resultSize;
		bottomLevelAccelerationStructureGenerator.ComputeBufferSizes(device, scratchSize, resultSize);

		buffers = AccelerationStructureBuffers(device, scratchSize, resultSize);

		bottomLevelAccelerationStructureGenerator.Generate(m_deviceResources->GetCommandList(), buffers.Scratch.Get(), buffers.Result.Get());
	}

	void CreateTopLevelAccelerationStructure(bool updateOnly) {
		TopLevelAccelerationStructureGenerator topLevelAccelerationStructureGenerator(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE);

		for (UINT size = static_cast<UINT>(m_renderItems.size()), i = 0; i < size; i++) {
			auto& renderItem = m_renderItems[i];

			const auto& shape = *renderItem.Shape;

			PxVec3 scaling;
			const auto geometry = shape.getGeometry();
			switch (shape.getGeometryType()) {
			case PxGeometryType::eSPHERE: scaling = PxVec3(geometry.sphere().radius / m_geometries.Sphere.Radius); break;
			default: throw;
			}

			if (updateOnly) UpdateRenderItem(renderItem);

			PxMat44 world(PxVec4(1, 1, -1, 1));
			world *= PxShapeExt::getGlobalPose(shape, *shape.getActor());
			world.scale(PxVec4(scaling, 1));
			topLevelAccelerationStructureGenerator.AddInstance(m_accelerationStructureBuffers.BottomLevel.Sphere.Result->GetGPUVirtualAddress(), XMLoadFloat4x4(reinterpret_cast<const XMFLOAT4X4*>(world.front())), i, i, ~0, D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_FRONT_COUNTERCLOCKWISE);
		}

		const auto device = m_deviceResources->GetD3DDevice();

		const auto commandList = m_deviceResources->GetCommandList();

		auto& buffers = m_accelerationStructureBuffers.TopLevel;

		if (updateOnly) {
			const auto uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(buffers.Result.Get());
			commandList->ResourceBarrier(1, &uavBarrier);
		}
		else {
			UINT64 scratchSize, resultSize, instanceDescsSize;
			topLevelAccelerationStructureGenerator.ComputeBufferSizes(device, scratchSize, resultSize, instanceDescsSize);
			buffers = AccelerationStructureBuffers(device, scratchSize, resultSize, instanceDescsSize);
		}

		topLevelAccelerationStructureGenerator.Generate(commandList, buffers.Scratch.Get(), buffers.Result.Get(), buffers.InstanceDesc.Get(), updateOnly ? buffers.Result.Get() : nullptr);
	}

	void CreateAccelerationStructures() {
		const auto commandList = m_deviceResources->GetCommandList();

		ThrowIfFailed(commandList->Reset(m_deviceResources->GetCommandAllocator(), nullptr));

		CreateBottomLevelAccelerationStructure({ m_geometries.Sphere->GetGeometryDesc() }, m_accelerationStructureBuffers.BottomLevel.Sphere);

		CreateTopLevelAccelerationStructure(false);

		ThrowIfFailed(commandList->Close());

		m_deviceResources->GetCommandQueue()->ExecuteCommandLists(1, CommandListCast(&commandList));

		m_deviceResources->WaitForGpu();
	}

	void CreateShaderBindingTables() {
		m_shaderBindingTableGenerator = {};

		m_shaderBindingTableGenerator.AddRayGenerationProgram(ShaderEntryPoints::RayGeneration, { nullptr });

		m_shaderBindingTableGenerator.AddMissProgram(ShaderEntryPoints::RadianceRayMiss, { nullptr });

		for (const auto& renderItem : m_renderItems) {
			D3D12_GPU_VIRTUAL_ADDRESS pColorTexture = NULL, pNormalTexture = NULL;
			if (renderItem.pTextures != nullptr) {
				const auto& textures = get<0>(*renderItem.pTextures);
				if (textures.contains(TextureType::ColorMap)) {
					pColorTexture = m_resourceDescriptors->GetGpuHandle(textures.at(TextureType::ColorMap).DescriptorHeapIndex).ptr;
				}
				if (textures.contains(TextureType::NormalMap)) {
					pNormalTexture = m_resourceDescriptors->GetGpuHandle(textures.at(TextureType::NormalMap).DescriptorHeapIndex).ptr;
				}
			}

			m_shaderBindingTableGenerator.AddHitGroup(
				renderItem.HitGroup.c_str(),
				{
					reinterpret_cast<void*>(m_resourceDescriptors->GetGpuHandle(renderItem.DescriptorHeapIndices.Vertices).ptr),
					reinterpret_cast<void*>(m_resourceDescriptors->GetGpuHandle(renderItem.DescriptorHeapIndices.Indices).ptr),
					reinterpret_cast<void*>(pColorTexture),
					reinterpret_cast<void*>(pNormalTexture),
					reinterpret_cast<void*>(m_constantBuffers.Objects.GpuAddress(renderItem.ConstantBufferIndices.Object))
				}
			);
		}

		const auto device = m_deviceResources->GetD3DDevice();

		ThrowIfFailed(CreateUploadBuffer(device, m_shaderBindingTableGenerator.ComputeSBTSize(), m_shaderBindingTable.ReleaseAndGetAddressOf()));

		ComPtr<ID3D12StateObjectProperties> stateObjectProperties;
		ThrowIfFailed(m_pipelineStateObject->QueryInterface(IID_PPV_ARGS(&stateObjectProperties)));

		m_shaderBindingTableGenerator.Generate(m_shaderBindingTable.Get(), stateObjectProperties.Get());
	}

	void ProcessInputs() {
		const auto gamepadState = m_inputDevices.Gamepad->GetState(0);
		auto& gamepadStateTracker = m_inputDeviceStateTrackers.Gamepad;
		gamepadStateTracker.Update(gamepadState);

		const auto keyboardState = m_inputDevices.Keyboard->GetState();
		auto& keyboardStateTracker = m_inputDeviceStateTrackers.Keyboard;
		keyboardStateTracker.Update(keyboardState);

		const auto mouseState = m_inputDevices.Mouse->GetState();
		auto& mouseStateTracker = m_inputDeviceStateTrackers.Mouse;
		mouseStateTracker.Update(mouseState);

		{
			if (gamepadStateTracker.menu == GamepadButtonState::PRESSED) m_isMenuOpen = !m_isMenuOpen;
			if (keyboardStateTracker.IsKeyPressed(Key::Escape)) m_isMenuOpen = !m_isMenuOpen;
		}

		if (m_isMenuOpen) m_inputDevices.Mouse->SetMode(Mouse::MODE_ABSOLUTE);
		else {
			{
				if (gamepadStateTracker.view == GamepadButtonState::PRESSED) m_isPhysicsSimulationRunning = !m_isPhysicsSimulationRunning;
				if (keyboardStateTracker.IsKeyPressed(Key::Tab)) m_isPhysicsSimulationRunning = !m_isPhysicsSimulationRunning;
			}

			{
				auto& isGravityEnabled = get<1>(m_physicsObjects.RigidBodies.at(Objects::Earth));
				if (gamepadStateTracker.x == GamepadButtonState::PRESSED) isGravityEnabled = !isGravityEnabled;
				if (keyboardStateTracker.IsKeyPressed(Key::G)) isGravityEnabled = !isGravityEnabled;
			}

			{
				auto& isGravityEnabled = get<1>(m_physicsObjects.RigidBodies.at(Objects::Star));
				if (gamepadStateTracker.b == GamepadButtonState::PRESSED) isGravityEnabled = !isGravityEnabled;
				if (keyboardStateTracker.IsKeyPressed(Key::H)) isGravityEnabled = !isGravityEnabled;
			}

			m_inputDevices.Mouse->SetMode(Mouse::MODE_RELATIVE);

			UpdateCamera(gamepadState, keyboardState, mouseState);
		}
	}

	void UpdateCamera(const GamePad::State& gamepadState, const Keyboard::State& keyboardState, const Mouse::State& mouseState) {
		const auto elapsedSeconds = static_cast<float>(m_stepTimer.GetElapsedSeconds());

		const auto Translate = [&](const XMFLOAT3& displacement) {
			if (!displacement.x && !displacement.y && !displacement.z) return;

			constexpr auto To_PxVec3 = [](const XMFLOAT3& value) { return PxVec3(value.x, value.y, -value.z); };

			const auto& directions = m_firstPersonCamera.GetDirections();
			auto x = To_PxVec3(directions.Right * displacement.x + directions.Up * displacement.y + directions.Forward * displacement.z);
			const auto magnitude = x.magnitude();
			const auto normalized = x / magnitude;

			PxRaycastBuffer raycastBuffer;
			if (m_myPhysX.GetScene().raycast(To_PxVec3(m_firstPersonCamera.GetPosition()), normalized, magnitude + 0.1f, raycastBuffer) && raycastBuffer.block.distance < magnitude) {
				x = normalized * max(0.0f, raycastBuffer.block.distance - 0.1f);
			}

			m_firstPersonCamera.Translate({ x.x, x.y, -x.z });
		};

		const auto Pitch = [&](float angle) {
			const auto pitch = asin(m_firstPersonCamera.GetDirections().Forward.y);
			if (pitch - angle > XM_PIDIV2) angle = -max(0.0f, XM_PIDIV2 - pitch - 0.1f);
			else if (pitch - angle < -XM_PIDIV2) angle = -min(0.0f, XM_PIDIV2 + pitch + 0.1f);
			m_firstPersonCamera.Pitch(angle);
		};

		if (gamepadState.IsConnected()) {
			const auto translationSpeed = elapsedSeconds * 15 * (gamepadState.IsLeftTriggerPressed() ? 0.5f : 1) * (gamepadState.IsRightTriggerPressed() ? 2.0f : 1);
			const XMFLOAT3 displacement{ gamepadState.thumbSticks.leftX * translationSpeed, 0, gamepadState.thumbSticks.leftY * translationSpeed };
			Translate(displacement);

			const auto rotationSpeed = elapsedSeconds * XM_2PI * 0.4f;
			m_firstPersonCamera.Yaw(gamepadState.thumbSticks.rightX * rotationSpeed);
			Pitch(gamepadState.thumbSticks.rightY * rotationSpeed);
		}

		if (mouseState.positionMode == Mouse::MODE_RELATIVE) {
			const auto translationSpeed = elapsedSeconds * 15 * (keyboardState.LeftControl ? 0.5f : 1) * (keyboardState.LeftShift ? 2.0f : 1);
			XMFLOAT3 displacement{};
			if (keyboardState.A) displacement.x -= translationSpeed;
			if (keyboardState.D) displacement.x += translationSpeed;
			if (keyboardState.W) displacement.z += translationSpeed;
			if (keyboardState.S) displacement.z -= translationSpeed;
			Translate(displacement);

			const auto rotationSpeed = elapsedSeconds * 20;
			m_firstPersonCamera.Yaw(XMConvertToRadians(static_cast<float>(mouseState.x)) * rotationSpeed);
			Pitch(XMConvertToRadians(static_cast<float>(mouseState.y)) * rotationSpeed);
		}
	}

	void UpdateSceneConstantBuffer() {
		auto& sceneConstants = *m_constantBuffers.Scene.Memory();

		sceneConstants.FrameCount = m_stepTimer.GetFrameCount();

		auto& camera = sceneConstants.Camera;
		camera.Position = m_firstPersonCamera.GetPosition();
		camera.ProjectionToWorld = XMMatrixTranspose(XMMatrixInverse(nullptr, m_firstPersonCamera.GetView() * m_firstPersonCamera.GetProjection()));
	}

	void UpdateRenderItem(RenderItem& renderItem) const {
		const auto& shape = *renderItem.Shape;

		const auto rigidBody = renderItem.Shape->getActor()->is<PxRigidBody>();
		if (rigidBody == nullptr) return;

		const auto mass = rigidBody->getMass();
		if (!mass) return;

		const auto& position = PxShapeExt::getGlobalPose(shape, *shape.getActor()).p;

		if (const auto& spring = m_physicsObjects.Spring;
			renderItem.Name == Objects::HarmonicOscillator) {
			const auto k = SimpleHarmonicMotion::Spring::CalculateConstant(mass, spring.Period);
			const PxVec3 x(0, position.y - spring.PositionY, 0);
			rigidBody->addForce(-k * x);
		}

		if (const auto& earth = m_physicsObjects.RigidBodies.at(Objects::Earth);
			(get<1>(earth) && renderItem.Name != Objects::Earth)
			|| renderItem.Name == Objects::Moon) {
			const auto& earthRigidBody = *get<0>(earth);
			const auto x = earthRigidBody.getGlobalPose().p - position;
			const auto magnitude = x.magnitude();
			const auto normalized = x / magnitude;
			rigidBody->addForce(UniversalGravitation::CalculateAccelerationMagnitude(earthRigidBody.getMass(), magnitude) * normalized, PxForceMode::eACCELERATION);
		}

		if (const auto& star = m_physicsObjects.RigidBodies.at(Objects::Star);
			get<1>(star) && renderItem.Name != Objects::Star) {
			const auto x = get<0>(star)->getGlobalPose().p - position;
			const auto normalized = x.getNormalized();
			rigidBody->addForce(10 * normalized, PxForceMode::eACCELERATION);
		}
	}

	void DispatchRays() {
		if (m_isPhysicsSimulationRunning) CreateTopLevelAccelerationStructure(true);

		const auto commandList = m_deviceResources->GetCommandList();

		commandList->SetComputeRootSignature(m_globalRootSignature.Get());

		const auto scopedBarrier = m_constantBuffers.Scene.Memory()->IsEnvironmentCubeMapUsed ?
			make_unique<ScopedBarrier>(
				commandList,
				initializer_list<D3D12_RESOURCE_BARRIER>{
			CD3DX12_RESOURCE_BARRIER::Transition(get<0>(m_textures[Objects::Environment])[TextureType::CubeMap].Resource.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
		}
		) : nullptr;

		commandList->SetComputeRootDescriptorTable(0, m_resourceDescriptors->GetGpuHandle(DescriptorHeapIndex::CurrentOutputUAV));
		commandList->SetComputeRootShaderResourceView(1, m_accelerationStructureBuffers.TopLevel.Result->GetGPUVirtualAddress());
		commandList->SetComputeRootConstantBufferView(2, m_constantBuffers.Scene.GpuAddress());
		commandList->SetComputeRootDescriptorTable(3, m_resourceDescriptors->GetGpuHandle(DescriptorHeapIndex::EnvironmentCubeMap));

		commandList->SetPipelineState1(m_pipelineStateObject.Get());

		const auto
			rayGenerationSectionSize = m_shaderBindingTableGenerator.GetRayGenerationSectionSize(),
			missSectionSize = m_shaderBindingTableGenerator.GetMissSectionSize(),
			hitGroupSectionSize = m_shaderBindingTableGenerator.GetHitGroupSectionSize();
		const auto
			pRayGenerationShaderRecord = m_shaderBindingTable->GetGPUVirtualAddress(),
			pMissShaderTable = pRayGenerationShaderRecord + rayGenerationSectionSize,
			pHitGroupTable = pMissShaderTable + missSectionSize;

		const auto outputSize = GetOutputSize();

		const D3D12_DISPATCH_RAYS_DESC dispatchRaysDesc{
			.RayGenerationShaderRecord = {
				.StartAddress = pRayGenerationShaderRecord,
				.SizeInBytes = rayGenerationSectionSize
			},
			.MissShaderTable = {
				.StartAddress = pMissShaderTable,
				.SizeInBytes = missSectionSize,
				.StrideInBytes = m_shaderBindingTableGenerator.GetMissEntrySize()
			},
			.HitGroupTable = {
				.StartAddress = pHitGroupTable,
				.SizeInBytes = hitGroupSectionSize,
				.StrideInBytes = m_shaderBindingTableGenerator.GetHitGroupEntrySize()
			},
			.Width = static_cast<UINT>(outputSize.cx),
			.Height = static_cast<UINT>(outputSize.cy),
			.Depth = 1
		};

		commandList->DispatchRays(&dispatchRaysDesc);
	}

	void RenderMenu() {
		ImGui_ImplDX12_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		{
			const auto outputSize = GetOutputSize();

			ImGui::SetNextWindowPos({});
			ImGui::SetNextWindowSize({ static_cast<float>(outputSize.cx), 0 });

			ImGui::Begin("##Menu", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBackground);

			if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) ImGui::SetWindowFocus();

			if (ImGui::CollapsingHeader("Graphics Settings")) {
				{
					{
						constexpr LPCSTR WindowModes[]{ "Windowed", "Borderless", "Fullscreen" };
						auto windowMode = m_windowModeHelper->GetMode();
						if (ImGui::Combo("Window Mode", reinterpret_cast<int*>(&windowMode), WindowModes, ARRAYSIZE(WindowModes))) {
							m_windowModeHelper->SetMode(windowMode);
						}
					}

					{
						constexpr auto ToString = [](const SIZE& size) { return to_string(size.cx) + " × " + to_string(size.cy); };

						if (ImGui::BeginCombo("Resolution", ToString(m_windowModeHelper->Resolution).c_str())) {
							for (const auto& displayResolution : g_displayResolutions) {
								const auto isSelected = m_windowModeHelper->Resolution == displayResolution;
								if (ImGui::Selectable(ToString(displayResolution).c_str(), isSelected)) {
									m_windowModeHelper->Resolution = displayResolution;
								}
								if (isSelected) ImGui::SetItemDefaultFocus();
							}

							ImGui::EndCombo();
						}
					}
				}

				ImGui::Separator();

				if (ImGui::SliderInt("Raytracing Samples Per Pixel", reinterpret_cast<int*>(&m_raytracingSamplesPerPixel), 1, static_cast<int>(MaxRaytracingSamplesPerPixel), "%d", ImGuiSliderFlags_NoInput)) {
					m_constantBuffers.Scene.Memory()->RaytracingSamplesPerPixel = m_raytracingSamplesPerPixel;

					GraphicsSettingsData::Save<GraphicsSettingsData::Raytracing::SamplesPerPixel>(m_raytracingSamplesPerPixel);
				}

				ImGui::Separator();

				if (ImGui::Checkbox("Temporal Anti-Aliasing", &m_isTemporalAntiAliasingEnabled)) {
					GraphicsSettingsData::Save<GraphicsSettingsData::TemporalAntiAliasing::IsEnabled>(m_isTemporalAntiAliasingEnabled);
				}

				if (ImGui::SliderFloat("Alpha", &m_temporalAntiAliasingConstants.Alpha, 0, 1, "%.3f", ImGuiSliderFlags_NoInput)) {
					m_temporalAntiAliasingEffect->Constants.Alpha = m_temporalAntiAliasingConstants.Alpha;

					GraphicsSettingsData::Save<GraphicsSettingsData::TemporalAntiAliasing::Alpha>(m_temporalAntiAliasingConstants.Alpha);
				}

				if (ImGui::SliderFloat("Color-Box Sigma", &m_temporalAntiAliasingConstants.ColorBoxSigma, 0, MaxTemporalAntiAliasingColorBoxSigma, "%.3f", ImGuiSliderFlags_NoInput)) {
					m_temporalAntiAliasingEffect->Constants.ColorBoxSigma = m_temporalAntiAliasingConstants.ColorBoxSigma;

					GraphicsSettingsData::Save<GraphicsSettingsData::TemporalAntiAliasing::ColorBoxSigma>(m_temporalAntiAliasingConstants.ColorBoxSigma);
				}
			}

			if (ImGui::CollapsingHeader("Controls")) {
				const auto AddControls = [](LPCSTR treeLabel, LPCSTR tableID, const initializer_list<pair<LPCSTR, LPCSTR>>& controls) {
					if (ImGui::TreeNode(treeLabel)) {
						if (ImGui::BeginTable(tableID, 2, ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInner)) {
							for (const auto& control : controls) {
								ImGui::TableNextRow();

								ImGui::TableSetColumnIndex(0);
								ImGui::Text(control.first);

								ImGui::TableSetColumnIndex(1);
								ImGui::Text(control.second);
							}

							ImGui::EndTable();
						}

						ImGui::TreePop();
					}
				};

				AddControls(
					"Xbox Controller",
					"##XboxController",
					{
						{ "Menu", "Open/close menu" },
						{ "View", "Run/pause physics simulation" },
						{ "LS (rotate)", "Move" },
						{ "LT (hold)", "Move slower" },
						{ "RT (hold)", "Move faster" },
						{ "RS (rotate)", "Look around" },
						{ "X", "Toggle gravity of Earth" },
						{ "B", "Toggle gravity of the star" }
					}
				);

				AddControls(
					"Keyboard",
					"##Keyboard",
					{
						{ "Alt + Enter", "Toggle between windowed/borderless and fullscreen modes" },
						{ "Esc", "Open/close menu" },
						{ "Tab", "Run/pause physics simulation" },
						{ "W A S D", "Move" },
						{ "Left Ctrl (hold)", "Move slower" },
						{ "Left Shift (hold)", "Move faster" },
						{ "G", "Toggle gravity of Earth" },
						{ "H", "Toggle gravity of the star" }
					}
				);

				AddControls(
					"Mouse",
					"Mouse",
					{
						{ "(Move)", "Look around" }
					}
				);
			}

			if (ImGui::CollapsingHeader("About")) {
				ImGui::Text("© Hydr10n. All rights reserved.");

				constexpr auto URL = "https://github.com/Hydr10n/DirectX-Raytracing-Spheres-Demo";
				if (ImGuiEx::Hyperlink("GitHub repository", URL)) ShellExecuteA(nullptr, "open", URL, nullptr, nullptr, SW_SHOW);
			}

			{
				ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, { 0, 0.5f });
				if (ImGui::Button("Exit", { -FLT_MIN, 0 })) PostQuitMessage(ERROR_SUCCESS);
				ImGui::PopStyleVar();
			}

			ImGui::End();
		}

		ImGui::Render();
		ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), m_deviceResources->GetCommandList());
	}
};

D3DApp::D3DApp(const shared_ptr<WindowModeHelper>& windowModeHelper) : m_impl(make_unique<Impl>(windowModeHelper)) {}

D3DApp::~D3DApp() = default;

SIZE D3DApp::GetOutputSize() const noexcept { return m_impl->GetOutputSize(); }

void D3DApp::Tick() { m_impl->Tick(); }

void D3DApp::OnWindowSizeChanged() { m_impl->OnWindowSizeChanged(); }

void D3DApp::OnDisplayChanged() { m_impl->OnDisplayChanged(); }

void D3DApp::OnResuming() { m_impl->OnResuming(); }

void D3DApp::OnSuspending() { m_impl->OnSuspending(); }

void D3DApp::OnActivated() { return m_impl->OnActivated(); }

void D3DApp::OnDeactivated() { return m_impl->OnDeactivated(); }
