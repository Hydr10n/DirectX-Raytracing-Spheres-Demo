module;

#include "pch.h"

#include "DeviceResources.h"

#include "StepTimer.h"

#include "RenderTexture.h"

#include "directxtk12/GraphicsMemory.h"

#include "directxtk12/GamePad.h"
#include "directxtk12/Keyboard.h"
#include "directxtk12/Mouse.h"

#include "directxtk12/DirectXHelpers.h"

#include "directxtk12/DescriptorHeap.h"

#include "directxtk12/GeometricPrimitive.h"

#include "directxtk12/SimpleMath.h"

#include "MyPhysX.h"

#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"
#include "ImGuiEx.h"

#include "MyAppData.h"

#include <shellapi.h>

#include "Shaders/Raytracing.hlsl.h"

module App;

import Camera;
import DirectX.BufferHelpers;
import DirectX.PostProcess.TemporalAntiAliasing;
import DirectX.RaytracingHelpers;
import Material;
import Random;
import ShaderCommonData;
import SharedData;
import Texture;

using namespace DirectX;
using namespace DirectX::BufferHelpers;
using namespace DirectX::PostProcess;
using namespace DirectX::RaytracingHelpers;
using namespace DX;
using namespace Microsoft::WRL;
using namespace PhysicsHelpers;
using namespace physx;
using namespace std;
using namespace std::filesystem;
using namespace WindowHelpers;

using GamepadButtonState = GamePad::ButtonStateTracker::ButtonState;
using Key = Keyboard::Keys;

namespace {
	constexpr auto& GraphicsSettings = MyAppData::Settings::Graphics;
	constexpr auto& UISettings = MyAppData::Settings::UI;
	constexpr auto& ControlsSettings = MyAppData::Settings::Controls;
}

#define MAKE_OBJECT_NAME(Name) static constexpr LPCSTR Name = #Name;

struct App::Impl : IDeviceNotify {
	Impl(const shared_ptr<WindowModeHelper>& windowModeHelper) noexcept(false) : m_windowModeHelper(windowModeHelper) {
		{
			{
				{
					auto& cameraSettings = GraphicsSettings.Camera;
					cameraSettings.VerticalFieldOfView = clamp(cameraSettings.VerticalFieldOfView, CameraMinVerticalFieldOfView, CameraMaxVerticalFieldOfView);
				}

				{
					auto& raytracingSettings = GraphicsSettings.Raytracing;
					raytracingSettings.MaxTraceRecursionDepth = clamp(raytracingSettings.MaxTraceRecursionDepth, 1u, RaytracingMaxTraceRecursionDepth);
					raytracingSettings.SamplesPerPixel = clamp(raytracingSettings.SamplesPerPixel, 1u, RaytracingMaxSamplesPerPixel);
				}

				{
					auto& temporalAntiAliasing = GraphicsSettings.TemporalAntiAliasing;
					temporalAntiAliasing.Alpha = clamp(temporalAntiAliasing.Alpha, 0.0f, 1.0f);
					temporalAntiAliasing.ColorBoxSigma = clamp(temporalAntiAliasing.ColorBoxSigma, 0.0f, TemporalAntiAliasingMaxColorBoxSigma);
				}
			}

			{
				auto& menuSettings = UISettings.Menu;
				UISettings.Menu.BackgroundOpacity = clamp(menuSettings.BackgroundOpacity, 0.0f, 1.0f);
			}

			{
				auto& speedSettings = ControlsSettings.Camera.Speed;
				speedSettings.Movement = clamp(speedSettings.Movement, CameraMinMovementSpeed, CameraMaxMovementSpeed);
				speedSettings.Rotation = clamp(speedSettings.Rotation, CameraMinRotationSpeed, CameraMaxRotationSpeed);
			}
		}

		windowModeHelper->SetFullscreenResolutionHandledByWindow(false);

		{
			ImGui::CreateContext();

			ImGui::StyleColorsDark();

			auto& IO = ImGui::GetIO();

			IO.IniFilename = IO.LogFilename = nullptr;

			IO.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
			IO.BackendFlags |= ImGuiBackendFlags_HasGamepad;

			ImGui_ImplWin32_Init(m_windowModeHelper->hWnd);
		}

		m_firstPersonCamera.SetPosition({ 0, 0, -15 });

		{
			BuildTextures();

			BuildRenderItems();
		}

		{
			m_deviceResources->RegisterDeviceNotify(this);

			m_deviceResources->SetWindow(windowModeHelper->hWnd, windowModeHelper->GetResolution());

			m_deviceResources->CreateDeviceResources();
			CreateDeviceDependentResources();

			m_deviceResources->CreateWindowSizeDependentResources();
			CreateWindowSizeDependentResources();
		}

		m_inputDevices.Mouse->SetWindow(windowModeHelper->hWnd);
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

	float GetOutputAspectRatio() const noexcept {
		const auto outputSize = GetOutputSize();
		return static_cast<float>(outputSize.cx) / static_cast<float>(outputSize.cy);
	}

	void Tick() {
		m_stepTimer.Tick([&] { Update(); });

		Render();

		m_isViewChanged = false;

		if (m_isWindowSettingChanged) {
			ThrowIfFailed(m_windowModeHelper->Apply());
			m_isWindowSettingChanged = false;
		}
	}

	void OnWindowSizeChanged() {
		if (m_deviceResources->WindowSizeChanged(m_windowModeHelper->GetResolution())) {
			CreateWindowSizeDependentResources();
		}
	}

	void OnDisplayChanged() { m_deviceResources->UpdateColorSpace(); }

	void OnResuming() {
		m_stepTimer.ResetElapsedTime();

		m_inputDevices.Gamepad->Resume();

		m_inputDeviceStateTrackers = {};
	}

	void OnSuspending() { m_inputDevices.Gamepad->Suspend(); }

	void OnActivated() {
		m_inputDeviceStateTrackers.Keyboard.Reset();
		m_inputDeviceStateTrackers.Mouse.Reset();
	}

	void OnDeactivated() {}

	void OnDeviceLost() override {
		ImGui_ImplDX12_Shutdown();

		m_renderTextures = {};

		for (auto& [_, textures] : m_textures) for (auto& [_, texture] : get<0>(textures)) texture.Resource.Reset();

		m_topLevelAccelerationStructure.reset();
		m_bottomLevelAccelerationStructures = {};

		m_triangleMeshes = {};

		m_shaderBuffers = {};

		m_temporalAntiAliasing.reset();

		m_pipelineState.Reset();

		m_rootSignature.Reset();

		m_resourceDescriptorHeap.reset();

		m_graphicsMemory.reset();
	}

	void OnDeviceRestored() override {
		CreateDeviceDependentResources();

		CreateWindowSizeDependentResources();
	}

private:
	const shared_ptr<WindowModeHelper> m_windowModeHelper;

	unique_ptr<DeviceResources> m_deviceResources = make_unique<DeviceResources>(D3D12_RAYTRACING_TIER_1_1);

	StepTimer m_stepTimer;

	unique_ptr<GraphicsMemory> m_graphicsMemory;

	const struct {
		unique_ptr<GamePad> Gamepad = make_unique<DirectX::GamePad>();
		unique_ptr<Keyboard> Keyboard = make_unique<DirectX::Keyboard>();
		unique_ptr<Mouse> Mouse = make_unique<DirectX::Mouse>();
	} m_inputDevices;

	struct {
		GamePad::ButtonStateTracker Gamepad;
		Keyboard::KeyboardStateTracker Keyboard;
		Mouse::ButtonStateTracker Mouse;
	} m_inputDeviceStateTrackers;

	struct ObjectNames {
		MAKE_OBJECT_NAME(EnvironmentLight);
		MAKE_OBJECT_NAME(Environment);
		MAKE_OBJECT_NAME(Sphere);
		MAKE_OBJECT_NAME(AlienMetal);
		MAKE_OBJECT_NAME(Moon);
		MAKE_OBJECT_NAME(Earth);
		MAKE_OBJECT_NAME(Star);
		MAKE_OBJECT_NAME(HarmonicOscillator);
	};

	struct ResourceDescriptorHeapIndex {
		enum {
			LocalResourceDescriptorHeapIndices,
			Camera,
			GlobalData, LocalData,
			PreviousOutputSRV, CurrentOutputSRV, CurrentOutputUAV, MotionVectorsSRV, MotionVectorsUAV, FinalOutputUAV,
			SphereVertices, SphereIndices,
			EnvironmentLightCubeMap, EnvironmentCubeMap,
			AlienMetalBaseColorMap, AlienMetalMetallicMap, AlienMetalRoughnessMap, AlienMetalAmbientOcclusionMap, AlienMetalNormalMap,
			MoonBaseColorMap, MoonNormalMap,
			EarthBaseColorMap, EarthNormalMap,
			Font,
			Count
		};
	};
	unique_ptr<DescriptorPile> m_resourceDescriptorHeap;

	ComPtr<ID3D12RootSignature> m_rootSignature;

	ComPtr<ID3D12PipelineState> m_pipelineState;

	static constexpr float TemporalAntiAliasingMaxColorBoxSigma = 2;
	unique_ptr<TemporalAntiAliasing> m_temporalAntiAliasing;

	struct {
		unique_ptr<ConstantBuffer<GlobalResourceDescriptorHeapIndices>> GlobalResourceDescriptorHeapIndices;
		unique_ptr<ConstantBuffer<Camera>> Camera;
		unique_ptr<ConstantBuffer<GlobalData>> GlobalData;
		unique_ptr<StructuredBuffer<LocalResourceDescriptorHeapIndices>> LocalResourceDescriptorHeapIndices;
		unique_ptr<StructuredBuffer<LocalData>> LocalData;
	} m_shaderBuffers;

	map<string, shared_ptr<TriangleMesh<VertexPositionNormalTexture, UINT16>>, less<>> m_triangleMeshes;

	map<string, shared_ptr<BottomLevelAccelerationStructure>, less<>> m_bottomLevelAccelerationStructures;
	unique_ptr<TopLevelAccelerationStructure> m_topLevelAccelerationStructure;

	struct { unique_ptr<RenderTexture> PreviousOutput, CurrentOutput, MotionVectors, FinalOutput; } m_renderTextures;

	TextureDictionary m_textures;

	static constexpr float
		CameraMinVerticalFieldOfView = 30, CameraMaxVerticalFieldOfView = 120,
		CameraMinMovementSpeed = 0.1f, CameraMaxMovementSpeed = 100, CameraMinRotationSpeed = 0.01f, CameraMaxRotationSpeed = 5;
	bool m_isViewChanged{};
	FirstPersonCamera m_firstPersonCamera;

	struct RenderItem {
		string Name;
		struct {
			struct { UINT Vertices = ~0u, Indices = ~0u; } TriangleMesh;
		} ResourceDescriptorHeapIndices;
		Material Material{};
		TextureDictionary::mapped_type* pTextures{};
		PxShape* Shape{};
	};
	vector<RenderItem> m_renderItems;

	bool m_isSimulatingPhysics = true;
	struct {
		map<string, tuple<PxRigidBody*, bool /*IsGravityEnabled*/>, less<>> RigidBodies;
		const struct { PxReal PositionY = 0.5f, Period = 3; } Spring;
	} m_physicsObjects;
	MyPhysX m_myPhysX;

	static constexpr UINT RaytracingMaxTraceRecursionDepth = 16, RaytracingMaxSamplesPerPixel = 16;

	bool m_isMenuOpen = UISettings.Menu.IsOpenOnStartup;

	bool m_isWindowSettingChanged{};

	void CreateDeviceDependentResources() {
		const auto device = m_deviceResources->GetD3DDevice();

		m_graphicsMemory = make_unique<GraphicsMemory>(device);

		CreateDescriptorHeaps();

		CreateRootSignatures();

		CreatePipelineStates();

		CreatePostProcess();

		CreateGeometries();

		CreateAccelerationStructures();

		LoadTextures();

		CreateShaderBuffers();
	}

	void CreateWindowSizeDependentResources() {
		const auto device = m_deviceResources->GetD3DDevice();

		const auto outputSize = GetOutputSize();

		m_isViewChanged = true;

		{
			m_firstPersonCamera.SetLens(XMConvertToRadians(GraphicsSettings.Camera.VerticalFieldOfView), GetOutputAspectRatio());
			m_shaderBuffers.Camera->GetData().ProjectionToWorld = XMMatrixTranspose(m_firstPersonCamera.InverseViewProjection());
		}

		{
			const auto CreateResource = [&](DXGI_FORMAT format, unique_ptr<RenderTexture>& texture, UINT srvDescriptorHeapIndex = ~0u, UINT uavDescriptorHeapIndex = ~0u) {
				texture = make_unique<RenderTexture>(format);
				texture->SetDevice(device, m_resourceDescriptorHeap.get(), srvDescriptorHeapIndex, uavDescriptorHeapIndex);
				texture->CreateResource(outputSize.cx, outputSize.cy);
			};

			const auto backBufferFormat = m_deviceResources->GetBackBufferFormat();

			{
				const auto format = m_deviceResources->GetBackBufferFormat();
				CreateResource(format, m_renderTextures.PreviousOutput, ResourceDescriptorHeapIndex::PreviousOutputSRV);
				CreateResource(format, m_renderTextures.CurrentOutput, ResourceDescriptorHeapIndex::CurrentOutputSRV, ResourceDescriptorHeapIndex::CurrentOutputUAV);
				CreateResource(format, m_renderTextures.FinalOutput, ~0u, ResourceDescriptorHeapIndex::FinalOutputUAV);
			}

			CreateResource(DXGI_FORMAT_R32G32_FLOAT, m_renderTextures.MotionVectors, ResourceDescriptorHeapIndex::MotionVectorsSRV, ResourceDescriptorHeapIndex::MotionVectorsUAV);
		}

		m_temporalAntiAliasing->TextureSize = outputSize;

		{
			auto& IO = ImGui::GetIO();

			if (IO.BackendRendererUserData != nullptr) ImGui_ImplDX12_Shutdown();
			ImGui_ImplDX12_Init(device, static_cast<int>(m_deviceResources->GetBackBufferCount()), m_deviceResources->GetBackBufferFormat(), m_resourceDescriptorHeap->Heap(), m_resourceDescriptorHeap->GetCpuHandle(ResourceDescriptorHeapIndex::Font), m_resourceDescriptorHeap->GetGpuHandle(ResourceDescriptorHeapIndex::Font));

			IO.Fonts->Clear();
			IO.Fonts->AddFontFromFileTTF(R"(C:\Windows\Fonts\segoeui.ttf)", static_cast<float>(outputSize.cy) * 0.025f);
		}
	}

	void Update() {
		PIXBeginEvent(PIX_COLOR_DEFAULT, L"Update");

		ProcessInput();

		if (m_isSimulatingPhysics) {
			m_myPhysX.Tick(static_cast<PxReal>(min(1.0 / 60, m_stepTimer.GetElapsedSeconds())));

			m_isViewChanged = true;
		}

		UpdateGlobalData();

		PIXEndEvent();
	}

	void Render() {
		if (!m_stepTimer.GetFrameCount()) return;

		m_deviceResources->Prepare();

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

		PIXBeginEvent(commandList, PIX_COLOR_DEFAULT, L"Render");

		const auto descriptorHeaps = { m_resourceDescriptorHeap->Heap() };
		commandList->SetDescriptorHeaps(static_cast<UINT>(size(descriptorHeaps)), descriptorHeaps.begin());

		DispatchRays();

		{
			const auto isTemporalAntiAliasingEnabled = GraphicsSettings.TemporalAntiAliasing.IsEnabled && m_isViewChanged;

			if (isTemporalAntiAliasingEnabled) {
				const ScopedBarrier scopedBarrier(
					commandList,
					{
						CD3DX12_RESOURCE_BARRIER::Transition(m_renderTextures.PreviousOutput->GetResource(), m_renderTextures.PreviousOutput->GetCurrentState(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
						CD3DX12_RESOURCE_BARRIER::Transition(m_renderTextures.CurrentOutput->GetResource(), m_renderTextures.CurrentOutput->GetCurrentState(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
						CD3DX12_RESOURCE_BARRIER::Transition(m_renderTextures.MotionVectors->GetResource(), m_renderTextures.MotionVectors->GetCurrentState(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
					}
				);

				m_temporalAntiAliasing->Process(commandList);
			}

			{
				const auto renderTarget = m_deviceResources->GetRenderTarget();
				const auto& previousOutput = m_renderTextures.PreviousOutput, & output = isTemporalAntiAliasingEnabled ? m_renderTextures.FinalOutput : m_renderTextures.CurrentOutput;

				const ScopedBarrier scopedBarrier(
					commandList,
					{
						CD3DX12_RESOURCE_BARRIER::Transition(renderTarget, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST),
						CD3DX12_RESOURCE_BARRIER::Transition(previousOutput->GetResource(), previousOutput->GetCurrentState(), D3D12_RESOURCE_STATE_COPY_DEST),
						CD3DX12_RESOURCE_BARRIER::Transition(output->GetResource(), output->GetCurrentState(), D3D12_RESOURCE_STATE_COPY_SOURCE)
					}
				);

				commandList->CopyResource(renderTarget, output->GetResource());
				commandList->CopyResource(previousOutput->GetResource(), output->GetResource());
			}
		}

		if (m_isMenuOpen) RenderMenu();

		PIXEndEvent(commandList);

		PIXBeginEvent(PIX_COLOR_DEFAULT, L"Present");

		m_deviceResources->Present();

		m_deviceResources->WaitForGpu();

		m_graphicsMemory->Commit(m_deviceResources->GetCommandQueue());

		PIXEndEvent();
	}

	void BuildTextures() {
		decltype(m_textures) textures;

		textures.DirectoryPath = path(*__wargv).replace_filename(LR"(Assets\Textures)");

		textures[ObjectNames::EnvironmentLight] = {
			{
				{
					TextureType::CubeMap,
					Texture{
						.DescriptorHeapIndices{
							.SRV = ResourceDescriptorHeapIndex::EnvironmentLightCubeMap
						},
						.FilePath = L"Space.dds"
					}
				}
			},
			XMMatrixRotationRollPitchYaw(XM_PI, XM_PI * 0.2f, 0)
		};

		textures[ObjectNames::AlienMetal] = {
			{
				{
					TextureType::BaseColorMap,
					Texture{
						.DescriptorHeapIndices{
							.SRV = ResourceDescriptorHeapIndex::AlienMetalBaseColorMap
						},
						.FilePath = L"Alien-Metal_Albedo.png"
					}
				},
				{
					TextureType::NormalMap,
					Texture{
						.DescriptorHeapIndices{
							.SRV = ResourceDescriptorHeapIndex::AlienMetalNormalMap
						},
						.FilePath = L"Alien-Metal_Normal.png"
					}
				},
				{
					TextureType::MetallicMap,
					Texture{
						.DescriptorHeapIndices{
							.SRV = ResourceDescriptorHeapIndex::AlienMetalMetallicMap
						},
						.FilePath = L"Alien-Metal_Metallic.png"
					}
				},
				{
					TextureType::RoughnessMap,
					Texture{
						.DescriptorHeapIndices{
							.SRV = ResourceDescriptorHeapIndex::AlienMetalRoughnessMap
						},
						.FilePath = L"Alien-Metal_Roughness.png"
					}
				},
				{
					TextureType::AmbientOcclusionMap,
					Texture{
						.DescriptorHeapIndices{
							.SRV = ResourceDescriptorHeapIndex::AlienMetalAmbientOcclusionMap
						},
						.FilePath = L"Alien-Metal_AO.png"
					}
				}
			},
			XMMatrixIdentity()
		};

		textures[ObjectNames::Moon] = {
			{
				{
					TextureType::BaseColorMap,
					Texture{
						.DescriptorHeapIndices{
							.SRV = ResourceDescriptorHeapIndex::MoonBaseColorMap
						},
						.FilePath = L"Moon_BaseColor.jpg"
					}
				},
				{
					TextureType::NormalMap,
					Texture{
						.DescriptorHeapIndices{
							.SRV = ResourceDescriptorHeapIndex::MoonNormalMap
						},
						.FilePath = L"Moon_Normal.jpg"
					}
				}
			},
			XMMatrixTranslation(0.5f, 0, 0)
		};

		textures[ObjectNames::Earth] = {
			{
				{
					TextureType::BaseColorMap,
					Texture{
						.DescriptorHeapIndices{
							.SRV = ResourceDescriptorHeapIndex::EarthBaseColorMap
						},
						.FilePath = L"Earth_BaseColor.jpg"
					}
				},
				{
					TextureType::NormalMap,
					Texture{
						.DescriptorHeapIndices{
							.SRV = ResourceDescriptorHeapIndex::EarthNormalMap
						},
						.FilePath = L"Earth_Normal.jpg"
					}
				}
			},
			XMMatrixIdentity()
		};

		m_textures = move(textures);
	}

	void LoadTextures() {
		const auto device = m_deviceResources->GetD3DDevice();
		const auto commandQueue = m_deviceResources->GetCommandQueue();

		m_textures.Load(device, commandQueue, *m_resourceDescriptorHeap, 8);
	}

	void BuildRenderItems() {
		decltype(m_renderItems) renderItems;

		const auto& material = *m_myPhysX.GetPhysics().createMaterial(0.5f, 0.5f, 0.6f);

		const auto AddRenderItem = [&](RenderItem& renderItem, const auto& transform, const PxSphereGeometry& geometry) -> decltype(auto) {
			renderItem.ResourceDescriptorHeapIndices = {
				.TriangleMesh{
					.Vertices = ResourceDescriptorHeapIndex::SphereVertices,
					.Indices = ResourceDescriptorHeapIndex::SphereIndices
				}
			};

			auto& rigidDynamic = *m_myPhysX.GetPhysics().createRigidDynamic(PxTransform(transform));

			renderItem.Shape = PxRigidActorExt::createExclusiveShape(rigidDynamic, geometry, material);

			PxRigidBodyExt::updateMassAndInertia(rigidDynamic, 1);

			rigidDynamic.setAngularDamping(0);

			m_myPhysX.GetScene().addActor(rigidDynamic);

			renderItems.emplace_back(renderItem);

			return rigidDynamic;
		};

		{
			const struct {
				LPCSTR Name;
				PxVec3 Position;
				Material Material;
			} objects[]{
				{
					.Name = ObjectNames::AlienMetal,
					.Position{ -2, 0.5f, 0 },
					.Material{
						.BaseColor{ 0.1f, 0.2f, 0.5f, 1 },
						.Roughness = 0.9f
					}
				},
				{
					.Position{ 0, 0.5f, 0 },
					.Material{
						.BaseColor{ 1, 1, 1, 0 },
						.Roughness = 0,
						.RefractiveIndex = 1.5f,
					}
				},
				{
					.Position{ 0, 2, 0 },
					.Material{
						.BaseColor{ 1, 1, 1, 0 },
						.Roughness = 0.5f,
						.RefractiveIndex = 1.5f,
					}
				},
				{
					.Position{ 2, 0.5f, 0 },
					.Material{
						.BaseColor{ 0.7f, 0.6f, 0.5f, 1 },
						.Metallic = 1,
						.Roughness = 0.3f
					}
				}
			};
			for (const auto& [Name, Position, Material] : objects) {
				RenderItem renderItem;

				if (Name != nullptr && m_textures.contains(Name)) renderItem.pTextures = &m_textures.at(Name);

				renderItem.Material = Material;

				AddRenderItem(renderItem, Position, PxSphereGeometry(0.5f));
			}

			Random random;
			for (auto i : views::iota(-10, 11)) {
				for (auto j : views::iota(-10, 11)) {
					constexpr auto A = 0.5f;
					const auto omega = PxTwoPi / m_physicsObjects.Spring.Period;

					PxVec3 position;
					position.x = static_cast<float>(i) + 0.7f * random.Float();
					position.y = m_physicsObjects.Spring.PositionY + SimpleHarmonicMotion::Spring::CalculateDisplacement(A, omega, 0.0f, position.x);
					position.z = static_cast<float>(j) - 0.7f * random.Float();

					bool isOverlapping = false;
					for (const auto& [_, Position, Material] : objects) {
						if ((position - Position).magnitude() < 1) {
							isOverlapping = true;
							break;
						}
					}
					if (isOverlapping) continue;

					RenderItem renderItem;

					renderItem.Name = ObjectNames::HarmonicOscillator;

					constexpr auto RandomFloat4 = [&](float min) {
						const auto value = random.Float3();
						return XMFLOAT4(value.x, value.y, value.z, 1);
					};
					if (const auto randomValue = random.Float();
						randomValue < 0.3f) {
						renderItem.Material = { .BaseColor = RandomFloat4(0.1f) };
					}
					else if (randomValue < 0.6f) {
						renderItem.Material = {
							.BaseColor = RandomFloat4(0.1f),
							.Metallic = 1,
							.Roughness = random.Float(0, 0.5f)
						};
					}
					else if (randomValue < 0.9f) {
						renderItem.Material = {
							.BaseColor = RandomFloat4(0.1f),
							.Roughness = random.Float(0, 0.5f),
							.Opacity = 0,
							.RefractiveIndex = 1.5f
						};
					}
					else {
						renderItem.Material = {
							.BaseColor = RandomFloat4(0.1f),
							.EmissiveColor = RandomFloat4(0.2f),
							.Metallic = random.Float(0.4f),
							.Roughness = random.Float(0.3f)
						};
					}

					auto& rigidDynamic = AddRenderItem(renderItem, position, PxSphereGeometry(0.075f));
					rigidDynamic.setLinearVelocity({ 0, SimpleHarmonicMotion::Spring::CalculateVelocity(A, omega, 0.0f, position.x), 0 });
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
				.Name = ObjectNames::Moon,
				.Position{ -4, 4, 0 },
				.Radius = 0.25f,
				.OrbitalPeriod = 10,
				.Material{
					.BaseColor{ 0.5f, 0.5f, 0.5f, 1 },
					.Roughness = 0.8f
				}
			}, earth{
				.Name = ObjectNames::Earth,
				.Position{ 0, moon.Position.y, 0 },
				.Radius = 1,
				.RotationPeriod = 15,
				.Mass = UniversalGravitation::CalculateMass((moon.Position - earth.Position).magnitude(), moon.OrbitalPeriod),
				.Material{
					.BaseColor{ 0.3f, 0.4f, 0.5f, 1 },
					.Roughness = 0.8f
				}
			}, star{
				.Name = ObjectNames::Star,
				.Position{ 0, -50.1f, 0 },
				.Radius = 50,
				.Material{
					.BaseColor{ 0.5f, 0.5f, 0.5f, 1 },
					.Metallic = 1,
					.Roughness = 0
				}
			};
			for (const auto& [Name, Position, Radius, RotationPeriod, OrbitalPeriod, Mass, Material] : { moon, earth, star }) {
				RenderItem renderItem;

				renderItem.Name = Name;

				renderItem.Material = Material;

				if (m_textures.contains(Name)) renderItem.pTextures = &m_textures.at(Name);

				auto& rigidDynamic = AddRenderItem(renderItem, Position, PxSphereGeometry(Radius));
				if (renderItem.Name == ObjectNames::Moon) {
					const auto x = earth.Position - Position;
					const auto magnitude = x.magnitude();
					const auto normalized = x / magnitude;
					const auto linearSpeed = UniversalGravitation::CalculateFirstCosmicSpeed(earth.Mass, magnitude);
					rigidDynamic.setLinearVelocity(linearSpeed * PxVec3(-normalized.z, 0, normalized.x));
					rigidDynamic.setAngularVelocity({ 0, linearSpeed / magnitude, 0 });
				}
				else if (renderItem.Name == ObjectNames::Earth) {
					rigidDynamic.setAngularVelocity({ 0, PxTwoPi / RotationPeriod, 0 });
					PxRigidBodyExt::setMassAndUpdateInertia(rigidDynamic, &Mass, 1);
				}
				else if (renderItem.Name == ObjectNames::Star) rigidDynamic.setMass(0);

				m_physicsObjects.RigidBodies[Name] = { &rigidDynamic, false };
			}
		}

		m_renderItems = move(renderItems);
	}

	void CreateDescriptorHeaps() {
		const auto device = m_deviceResources->GetD3DDevice();

		m_resourceDescriptorHeap = make_unique<DescriptorPile>(device, ResourceDescriptorHeapIndex::Count);
	}

	void CreateRootSignatures() {
		const auto device = m_deviceResources->GetD3DDevice();

		ThrowIfFailed(device->CreateRootSignature(0, g_pRaytracing, size(g_pRaytracing), IID_PPV_ARGS(&m_rootSignature)));
	}

	void CreatePipelineStates() {
		const auto device = m_deviceResources->GetD3DDevice();

		const CD3DX12_SHADER_BYTECODE shaderByteCode(g_pRaytracing, size(g_pRaytracing));
		const D3D12_COMPUTE_PIPELINE_STATE_DESC computePipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS = shaderByteCode };
		ThrowIfFailed(device->CreateComputePipelineState(&computePipelineStateDesc, IID_PPV_ARGS(&m_pipelineState)));
	}

	void CreatePostProcess() {
		const auto device = m_deviceResources->GetD3DDevice();

		m_temporalAntiAliasing = make_unique<TemporalAntiAliasing>(device);
		m_temporalAntiAliasing->Constant = GraphicsSettings.TemporalAntiAliasing;
		m_temporalAntiAliasing->TextureDescriptors = {
			.PreviousOutputSRV = m_resourceDescriptorHeap->GetGpuHandle(ResourceDescriptorHeapIndex::PreviousOutputSRV),
			.CurrentOutputSRV = m_resourceDescriptorHeap->GetGpuHandle(ResourceDescriptorHeapIndex::CurrentOutputSRV),
			.MotionVectorsSRV = m_resourceDescriptorHeap->GetGpuHandle(ResourceDescriptorHeapIndex::MotionVectorsSRV),
			.FinalOutputUAV = m_resourceDescriptorHeap->GetGpuHandle(ResourceDescriptorHeapIndex::FinalOutputUAV)
		};
	}

	void CreateGeometries() {
		const auto device = m_deviceResources->GetD3DDevice();

		auto& sphere = m_triangleMeshes[ObjectNames::Sphere];

		GeometricPrimitive::VertexCollection vertices;
		GeometricPrimitive::IndexCollection indices;
		GeometricPrimitive::CreateGeoSphere(vertices, indices, 1, 6);

		sphere = make_shared<decay_t<decltype(sphere)>::element_type>(device, vertices, indices);
		sphere->CreateShaderResourceViews(m_resourceDescriptorHeap->GetCpuHandle(ResourceDescriptorHeapIndex::SphereVertices), m_resourceDescriptorHeap->GetCpuHandle(ResourceDescriptorHeapIndex::SphereIndices));
	}

	void CreateTopLevelAccelerationStructure(bool updateOnly) {
		vector<D3D12_RAYTRACING_INSTANCE_DESC> instanceDescs;
		instanceDescs.reserve(size(m_renderItems));
		for (UINT i = 0; auto & renderItem : m_renderItems) {
			const auto& shape = *renderItem.Shape;

			struct {
				LPCSTR Name;
				PxVec3 Scale;
			} object;
			switch (const auto geometry = shape.getGeometry(); shape.getGeometryType()) {
			case PxGeometryType::eSPHERE: {
				object = {
					.Name = ObjectNames::Sphere,
					.Scale = PxVec3(geometry.sphere().radius * 2)
				};
			} break;

			default: throw;
			}

			if (updateOnly) UpdateRenderItem(renderItem);

			PxMat44 world(PxVec4(1, 1, -1, 1));
			world *= PxShapeExt::getGlobalPose(shape, *shape.getActor());
			world.scale(PxVec4(object.Scale, 1));

			D3D12_RAYTRACING_INSTANCE_DESC instanceDesc;
			XMStoreFloat3x4(reinterpret_cast<XMFLOAT3X4*>(instanceDesc.Transform), XMLoadFloat4x4(reinterpret_cast<const XMFLOAT4X4*>(world.front())));
			instanceDesc.InstanceID = i++;
			instanceDesc.InstanceMask = ~0u;
			instanceDesc.InstanceContributionToHitGroupIndex = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
			instanceDesc.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_FRONT_COUNTERCLOCKWISE;
			instanceDesc.AccelerationStructure = m_bottomLevelAccelerationStructures.at(object.Name)->GetBuffer()->GetGPUVirtualAddress();
			instanceDescs.emplace_back(instanceDesc);
		}
		m_topLevelAccelerationStructure->Build(m_deviceResources->GetCommandList(), instanceDescs, updateOnly);
	}

	void CreateAccelerationStructures() {
		const auto device = m_deviceResources->GetD3DDevice();
		const auto commandList = m_deviceResources->GetCommandList();

		ThrowIfFailed(commandList->Reset(m_deviceResources->GetCommandAllocator(), nullptr));

		for (const auto& [name, mesh] : m_triangleMeshes) {
			auto& bottomLevelAccelerationStructure = m_bottomLevelAccelerationStructures[name];
			bottomLevelAccelerationStructure = make_shared<BottomLevelAccelerationStructure>(device, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE);
			bottomLevelAccelerationStructure->Build(m_deviceResources->GetCommandList(), initializer_list{ mesh->GetGeometryDesc() }, false);
		}

		m_topLevelAccelerationStructure = make_unique<TopLevelAccelerationStructure>(device, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE);
		CreateTopLevelAccelerationStructure(false);

		ThrowIfFailed(commandList->Close());

		m_deviceResources->GetCommandQueue()->ExecuteCommandLists(1, CommandListCast(&commandList));

		m_deviceResources->WaitForGpu();
	}

	void CreateShaderBuffers() {
		const auto device = m_deviceResources->GetD3DDevice();

		{
			const auto CreateBuffer = [&]<typename T>(unique_ptr<T>&uploadBuffer, const auto & data, UINT descriptorHeapIndex = ~0u) {
				uploadBuffer = make_unique<T>(device);
				uploadBuffer->GetData() = data;
				if (descriptorHeapIndex != ~0u) uploadBuffer->CreateConstantBufferView(m_resourceDescriptorHeap->GetCpuHandle(descriptorHeapIndex));
			};

			CreateBuffer(
				m_shaderBuffers.GlobalResourceDescriptorHeapIndices,
				GlobalResourceDescriptorHeapIndices{
					.LocalResourceDescriptorHeapIndices = ResourceDescriptorHeapIndex::LocalResourceDescriptorHeapIndices,
					.Camera = ResourceDescriptorHeapIndex::Camera,
					.GlobalData = ResourceDescriptorHeapIndex::GlobalData,
					.LocalData = ResourceDescriptorHeapIndex::LocalData,
					.Output = ResourceDescriptorHeapIndex::CurrentOutputUAV,
					.EnvironmentLightCubeMap = m_textures.contains(ObjectNames::EnvironmentLight) && get<0>(m_textures.at(ObjectNames::EnvironmentLight)).contains(TextureType::CubeMap) ? ResourceDescriptorHeapIndex::EnvironmentLightCubeMap : ~0u,
					.EnvironmentCubeMap = m_textures.contains(ObjectNames::Environment) && get<0>(m_textures.at(ObjectNames::Environment)).contains(TextureType::CubeMap) ? ResourceDescriptorHeapIndex::EnvironmentCubeMap : ~0u
				}
			);

			CreateBuffer(
				m_shaderBuffers.Camera,
				Camera{ .Position = m_firstPersonCamera.GetPosition() },
				ResourceDescriptorHeapIndex::Camera
			);

			CreateBuffer(
				m_shaderBuffers.GlobalData,
				GlobalData{
					.RaytracingMaxTraceRecursionDepth = GraphicsSettings.Raytracing.MaxTraceRecursionDepth,
					.RaytracingSamplesPerPixel = GraphicsSettings.Raytracing.SamplesPerPixel,
					.EnvironmentLightColor{ 0, 0, 0, -1 },
					.EnvironmentLightCubeMapTransform = m_textures.contains(ObjectNames::EnvironmentLight) ? XMMatrixTranspose(get<1>(m_textures.at(ObjectNames::EnvironmentLight))) : XMMatrixIdentity(),
					.EnvironmentColor{ 0, 0, 0, -1 },
					.EnvironmentCubeMapTransform = m_textures.contains(ObjectNames::Environment) ? XMMatrixTranspose(get<1>(m_textures.at(ObjectNames::Environment))) : XMMatrixIdentity()
				},
				ResourceDescriptorHeapIndex::GlobalData
			);
		}

		{
			const auto CreateBuffer = [&]<typename T>(unique_ptr<T>&uploadBuffer, UINT count, UINT descriptorHeapIndex) {
				uploadBuffer = make_unique<T>(device, count);
				uploadBuffer->CreateShaderResourceView(m_resourceDescriptorHeap->GetCpuHandle(descriptorHeapIndex));
			};

			const auto renderItemsSize = static_cast<UINT>(size(m_renderItems));

			CreateBuffer(m_shaderBuffers.LocalResourceDescriptorHeapIndices, renderItemsSize, ResourceDescriptorHeapIndex::LocalResourceDescriptorHeapIndices);

			CreateBuffer(m_shaderBuffers.LocalData, renderItemsSize, ResourceDescriptorHeapIndex::LocalData);

			for (auto i : views::iota(0u, renderItemsSize)) {
				const auto& renderItem = m_renderItems[i];

				auto& localResourceDescriptorHeapIndices = (*m_shaderBuffers.LocalResourceDescriptorHeapIndices)[i];

				localResourceDescriptorHeapIndices = {
					.TriangleMesh{
						.Vertices = renderItem.ResourceDescriptorHeapIndices.TriangleMesh.Vertices,
						.Indices = renderItem.ResourceDescriptorHeapIndices.TriangleMesh.Indices
					}
				};

				auto& localData = (*m_shaderBuffers.LocalData)[i];

				localData.Material = renderItem.Material;

				if (renderItem.pTextures != nullptr) {
					for (const auto& [textureType, texture] : get<0>(*renderItem.pTextures)) {
						auto& textures = localResourceDescriptorHeapIndices.Textures;
						UINT* p;
						switch (textureType) {
						case TextureType::BaseColorMap: p = &textures.BaseColorMap; break;
						case TextureType::EmissiveMap: p = &textures.EmissiveMap; break;
						case TextureType::SpecularMap: p = &textures.SpecularMap; break;
						case TextureType::MetallicMap: p = &textures.MetallicMap; break;
						case TextureType::RoughnessMap: p = &textures.RoughnessMap; break;
						case TextureType::AmbientOcclusionMap: p = &textures.AmbientOcclusionMap; break;
						case TextureType::OpacityMap: p = &textures.OpacityMap; break;
						case TextureType::NormalMap: p = &textures.NormalMap; break;
						default: p = nullptr; break;
						}
						if (p != nullptr) *p = texture.DescriptorHeapIndices.SRV;
					}
					localData.TextureTransform = XMMatrixTranspose(get<1>(*renderItem.pTextures));
				}
			}
		}
	}

	void ProcessInput() {
		const auto gamepadState = m_inputDevices.Gamepad->GetState(0);
		auto& gamepadStateTracker = m_inputDeviceStateTrackers.Gamepad;
		if (gamepadState.IsConnected()) gamepadStateTracker.Update(gamepadState);
		else gamepadStateTracker.Reset();

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
				if (gamepadStateTracker.view == GamepadButtonState::PRESSED) m_isSimulatingPhysics = !m_isSimulatingPhysics;
				if (keyboardStateTracker.IsKeyPressed(Key::Tab)) m_isSimulatingPhysics = !m_isSimulatingPhysics;
			}

			{
				auto& isGravityEnabled = get<1>(m_physicsObjects.RigidBodies.at(ObjectNames::Earth));
				if (gamepadStateTracker.x == GamepadButtonState::PRESSED) isGravityEnabled = !isGravityEnabled;
				if (keyboardStateTracker.IsKeyPressed(Key::G)) isGravityEnabled = !isGravityEnabled;
			}

			{
				auto& isGravityEnabled = get<1>(m_physicsObjects.RigidBodies.at(ObjectNames::Star));
				if (gamepadStateTracker.b == GamepadButtonState::PRESSED) isGravityEnabled = !isGravityEnabled;
				if (keyboardStateTracker.IsKeyPressed(Key::H)) isGravityEnabled = !isGravityEnabled;
			}

			m_inputDevices.Mouse->SetMode(Mouse::MODE_RELATIVE);

			UpdateCamera(gamepadState, keyboardState, mouseState);
		}
	}

	void UpdateCamera(const GamePad::State& gamepadState, const Keyboard::State& keyboardState, const Mouse::State& mouseState) {
		const auto elapsedSeconds = static_cast<float>(m_stepTimer.GetElapsedSeconds());

		const auto& [Right, Up, Forward] = m_firstPersonCamera.GetDirections();

		const auto Translate = [&](const XMFLOAT3& displacement) {
			if (displacement.x == 0 && displacement.y == 0 && displacement.z == 0) return;

			m_isViewChanged = true;

			constexpr auto ToPxVec3 = [](const XMFLOAT3& value) { return PxVec3(value.x, value.y, -value.z); };

			auto x = ToPxVec3(Right * displacement.x + Up * displacement.y + Forward * displacement.z);
			const auto magnitude = x.magnitude();
			const auto normalized = x / magnitude;

			if (PxRaycastBuffer raycastBuffer;
				m_myPhysX.GetScene().raycast(ToPxVec3(m_firstPersonCamera.GetPosition()), normalized, magnitude + 0.1f, raycastBuffer) && raycastBuffer.block.distance < magnitude) {
				x = normalized * max(0.0f, raycastBuffer.block.distance - 0.1f);
			}

			m_firstPersonCamera.Translate({ x.x, x.y, -x.z });
		};

		const auto Yaw = [&](float angle) {
			if (angle == 0) return;

			m_isViewChanged = true;

			m_firstPersonCamera.Yaw(angle);
		};

		const auto Pitch = [&](float angle) {
			if (angle == 0) return;

			m_isViewChanged = true;

			if (const auto pitch = asin(Forward.y);
				pitch - angle > XM_PIDIV2) angle = -max(0.0f, XM_PIDIV2 - pitch - 0.1f);
			else if (pitch - angle < -XM_PIDIV2) angle = -min(0.0f, XM_PIDIV2 + pitch + 0.1f);
			m_firstPersonCamera.Pitch(angle);
		};

		const auto& speed = ControlsSettings.Camera.Speed;

		if (gamepadState.IsConnected()) {
			const auto translationSpeed = elapsedSeconds * 10 * speed.Movement * (gamepadState.IsLeftTriggerPressed() ? 0.5f : 1) * (gamepadState.IsRightTriggerPressed() ? 2.0f : 1);
			const XMFLOAT3 displacement{ gamepadState.thumbSticks.leftX * translationSpeed, 0, gamepadState.thumbSticks.leftY * translationSpeed };
			Translate(displacement);

			const auto rotationSpeed = elapsedSeconds * XM_2PI * 0.4f * speed.Rotation;
			Yaw(gamepadState.thumbSticks.rightX * rotationSpeed);
			Pitch(gamepadState.thumbSticks.rightY * rotationSpeed);
		}

		if (mouseState.positionMode == Mouse::MODE_RELATIVE) {
			const auto translationSpeed = elapsedSeconds * 10 * speed.Movement * (keyboardState.LeftControl ? 0.5f : 1) * (keyboardState.LeftShift ? 2.0f : 1);
			XMFLOAT3 displacement{};
			if (keyboardState.A) displacement.x -= translationSpeed;
			if (keyboardState.D) displacement.x += translationSpeed;
			if (keyboardState.W) displacement.z += translationSpeed;
			if (keyboardState.S) displacement.z -= translationSpeed;
			Translate(displacement);

			const auto rotationSpeed = elapsedSeconds * 20 * speed.Rotation;
			Yaw(XMConvertToRadians(static_cast<float>(mouseState.x)) * rotationSpeed);
			Pitch(XMConvertToRadians(static_cast<float>(mouseState.y)) * rotationSpeed);
		}

		if (m_isViewChanged) {
			auto& camera = m_shaderBuffers.Camera->GetData();
			camera.Position = m_firstPersonCamera.GetPosition();
			camera.ProjectionToWorld = XMMatrixTranspose(m_firstPersonCamera.InverseViewProjection());
		}
	}

	void UpdateGlobalData() {
		auto& globalData = m_shaderBuffers.GlobalData->GetData();

		globalData.FrameCount = m_stepTimer.GetFrameCount();
		globalData.AccumulatedFrameIndex = m_isViewChanged ? 0 : globalData.AccumulatedFrameIndex + 1;
	}

	void UpdateRenderItem(RenderItem& renderItem) const {
		const auto& shape = *renderItem.Shape;

		const auto rigidBody = shape.getActor()->is<PxRigidBody>();
		if (rigidBody == nullptr) return;

		const auto mass = rigidBody->getMass();
		if (!mass) return;

		const auto& position = PxShapeExt::getGlobalPose(shape, *shape.getActor()).p;

		if (const auto& [PositionY, Period] = m_physicsObjects.Spring;
			renderItem.Name == ObjectNames::HarmonicOscillator) {
			const auto k = SimpleHarmonicMotion::Spring::CalculateConstant(mass, Period);
			const PxVec3 x(0, position.y - PositionY, 0);
			rigidBody->addForce(-k * x);
		}

		if (const auto& earth = m_physicsObjects.RigidBodies.at(ObjectNames::Earth);
			(get<1>(earth) && renderItem.Name != ObjectNames::Earth)
			|| renderItem.Name == ObjectNames::Moon) {
			const auto& earthRigidBody = *get<0>(earth);
			const auto x = earthRigidBody.getGlobalPose().p - position;
			const auto magnitude = x.magnitude();
			const auto normalized = x / magnitude;
			rigidBody->addForce(UniversalGravitation::CalculateAccelerationMagnitude(earthRigidBody.getMass(), magnitude) * normalized, PxForceMode::eACCELERATION);
		}

		if (const auto& star = m_physicsObjects.RigidBodies.at(ObjectNames::Star);
			get<1>(star) && renderItem.Name != ObjectNames::Star) {
			const auto x = get<0>(star)->getGlobalPose().p - position;
			const auto normalized = x.getNormalized();
			rigidBody->addForce(10 * normalized, PxForceMode::eACCELERATION);
		}
	}

	void DispatchRays() {
		if (m_isSimulatingPhysics) CreateTopLevelAccelerationStructure(true);

		const auto commandList = m_deviceResources->GetCommandList();

		commandList->SetComputeRootSignature(m_rootSignature.Get());
		commandList->SetComputeRootShaderResourceView(0, m_topLevelAccelerationStructure->GetBuffer()->GetGPUVirtualAddress());
		commandList->SetComputeRootConstantBufferView(1, m_shaderBuffers.GlobalResourceDescriptorHeapIndices->GetResource()->GetGPUVirtualAddress());

		commandList->SetPipelineState(m_pipelineState.Get());

		const auto outputSize = GetOutputSize();
		commandList->Dispatch(static_cast<UINT>((outputSize.cx + 16) / 16), static_cast<UINT>((outputSize.cy + 16) / 16), 1);
	}

	void RenderMenu() {
		ImGui_ImplDX12_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		{
			const auto outputSize = GetOutputSize();

			ImGui::SetNextWindowPos({});
			ImGui::SetNextWindowSize({ static_cast<float>(outputSize.cx), 0 });
			ImGui::SetNextWindowBgAlpha(UISettings.Menu.BackgroundOpacity);

			ImGui::Begin("Menu", &m_isMenuOpen, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_HorizontalScrollbar);

			if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) ImGui::SetWindowFocus();

			if (ImGui::CollapsingHeader("Settings")) {
				if (ImGui::TreeNode("Graphics")) {
					bool isChanged = false;

					{
						constexpr LPCSTR WindowModes[]{ "Windowed", "Borderless", "Fullscreen" };
						if (auto windowMode = m_windowModeHelper->GetMode();
							ImGui::Combo("Window Mode", reinterpret_cast<int*>(&windowMode), WindowModes, static_cast<int>(size(WindowModes)))) {
							m_windowModeHelper->SetMode(windowMode);

							GraphicsSettings.WindowMode = windowMode;
							isChanged = m_isWindowSettingChanged = true;
						}
					}

					{
						const auto ToString = [](const SIZE& value) { return format("{} × {}", value.cx, value.cy); };
						if (const auto resolution = m_windowModeHelper->GetResolution();
							ImGui::BeginCombo("Resolution", ToString(resolution).c_str())) {
							for (const auto& displayResolution : g_displayResolutions) {
								const auto isSelected = resolution == displayResolution;
								if (ImGui::Selectable(ToString(displayResolution).c_str(), isSelected)) {
									m_windowModeHelper->SetResolution(displayResolution);

									GraphicsSettings.Resolution = displayResolution;
									m_isWindowSettingChanged = isChanged = true;
								}
								if (isSelected) ImGui::SetItemDefaultFocus();
							}

							ImGui::EndCombo();
						}
					}

					if (ImGui::TreeNodeEx("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
						auto& cameraSettings = GraphicsSettings.Camera;

						if (ImGui::SliderFloat("Vertical Field of View", &cameraSettings.VerticalFieldOfView, CameraMinVerticalFieldOfView, CameraMaxVerticalFieldOfView, "%.1f", ImGuiSliderFlags_NoInput)) {
							m_firstPersonCamera.SetLens(XMConvertToRadians(cameraSettings.VerticalFieldOfView), GetOutputAspectRatio());
							m_shaderBuffers.Camera->GetData().ProjectionToWorld = XMMatrixTranspose(m_firstPersonCamera.InverseViewProjection());

							m_shaderBuffers.GlobalData->GetData().AccumulatedFrameIndex = 0;

							isChanged = true;
						}

						ImGui::TreePop();
					}

					if (ImGui::TreeNodeEx("Raytracing", ImGuiTreeNodeFlags_DefaultOpen)) {
						auto& raytracingSettings = GraphicsSettings.Raytracing;

						auto& globalData = m_shaderBuffers.GlobalData->GetData();

						if (ImGui::SliderInt("Max Trace Recursion Depth", reinterpret_cast<int*>(&raytracingSettings.MaxTraceRecursionDepth), 1, RaytracingMaxTraceRecursionDepth, "%d", ImGuiSliderFlags_NoInput)) {
							globalData.RaytracingMaxTraceRecursionDepth = raytracingSettings.MaxTraceRecursionDepth;
							globalData.AccumulatedFrameIndex = 0;

							isChanged = true;
						}

						if (ImGui::SliderInt("Samples Per Pixel", reinterpret_cast<int*>(&raytracingSettings.SamplesPerPixel), 1, static_cast<int>(RaytracingMaxSamplesPerPixel), "%d", ImGuiSliderFlags_NoInput)) {
							globalData.RaytracingSamplesPerPixel = raytracingSettings.SamplesPerPixel;
							globalData.AccumulatedFrameIndex = 0;

							isChanged = true;
						}

						ImGui::TreePop();
					}

					if (ImGui::TreeNodeEx("Temporal Anti-Aliasing", ImGuiTreeNodeFlags_DefaultOpen)) {
						auto& temporalAntiAliasingSettings = GraphicsSettings.TemporalAntiAliasing;

						if (ImGui::Checkbox("Enable", &temporalAntiAliasingSettings.IsEnabled)) isChanged = true;

						if (ImGui::SliderFloat("Alpha", &temporalAntiAliasingSettings.Alpha, 0, 1, "%.2f", ImGuiSliderFlags_NoInput)) {
							m_temporalAntiAliasing->Constant.Alpha = temporalAntiAliasingSettings.Alpha;

							isChanged = true;
						}

						if (ImGui::SliderFloat("Color-Box Sigma", &temporalAntiAliasingSettings.ColorBoxSigma, 0, TemporalAntiAliasingMaxColorBoxSigma, "%.2f", ImGuiSliderFlags_NoInput)) {
							m_temporalAntiAliasing->Constant.ColorBoxSigma = temporalAntiAliasingSettings.ColorBoxSigma;

							isChanged = true;
						}

						ImGui::TreePop();
					}

					ImGui::TreePop();

					if (isChanged) ignore = GraphicsSettings.Save();
				}

				if (ImGui::TreeNode("UI")) {
					bool isChanged = false;

					if (ImGui::TreeNodeEx("Menu", ImGuiTreeNodeFlags_DefaultOpen)) {
						auto& menuSettings = UISettings.Menu;

						isChanged |= ImGui::Checkbox("Open on Startup", &menuSettings.IsOpenOnStartup);

						isChanged |= ImGui::SliderFloat("Background Opacity", &menuSettings.BackgroundOpacity, 0, 1, "%.2f", ImGuiSliderFlags_NoInput);

						ImGui::TreePop();
					}

					ImGui::TreePop();

					if (isChanged) UISettings.Save();
				}

				{
					ImGui::PushID(0);

					if (ImGui::TreeNode("Controls")) {
						bool isChanged = false;

						if (ImGui::TreeNodeEx("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
							auto& cameraSettings = ControlsSettings.Camera;

							if (ImGui::TreeNodeEx("Speed", ImGuiTreeNodeFlags_DefaultOpen)) {
								isChanged |= ImGui::SliderFloat("Movement", &cameraSettings.Speed.Movement, CameraMinMovementSpeed, CameraMaxMovementSpeed, "%.1f", ImGuiSliderFlags_NoInput);

								isChanged |= ImGui::SliderFloat("Rotation", &cameraSettings.Speed.Rotation, CameraMinRotationSpeed, CameraMaxRotationSpeed, "%.2f", ImGuiSliderFlags_NoInput);

								ImGui::TreePop();
							}

							ImGui::TreePop();
						}

						ImGui::TreePop();

						if (isChanged) ControlsSettings.Save();
					}

					ImGui::PopID();
				}
			}

			if (ImGui::CollapsingHeader("Controls")) {
				const auto AddContents = [](LPCSTR treeLabel, LPCSTR tableID, const initializer_list<pair<LPCSTR, LPCSTR>>& list) {
					if (ImGui::TreeNode(treeLabel)) {
						if (ImGui::BeginTable(tableID, 2, ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInner)) {
							for (const auto& [first, second] : list) {
								ImGui::TableNextRow();

								ImGui::TableSetColumnIndex(0);
								ImGui::Text(first);

								ImGui::TableSetColumnIndex(1);
								ImGui::Text(second);
							}

							ImGui::EndTable();
						}

						ImGui::TreePop();
					}
				};

				AddContents(
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

				AddContents(
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

				AddContents(
					"Mouse",
					"##Mouse",
					{
						{ "(Move)", "Look around" }
					}
				);
			}

			if (ImGui::CollapsingHeader("About")) {
				ImGui::Text("© Hydr10n. All rights reserved.");

				if (constexpr auto URL = "https://github.com/Hydr10n/DirectX-Raytracing-Spheres-Demo";
					ImGuiEx::Hyperlink("GitHub repository", URL)) {
					ShellExecuteA(nullptr, "open", URL, nullptr, nullptr, SW_SHOW);
				}
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

App::App(const shared_ptr<WindowModeHelper>& windowModeHelper) : m_impl(make_unique<Impl>(windowModeHelper)) {}

App::~App() = default;

SIZE App::GetOutputSize() const noexcept { return m_impl->GetOutputSize(); }

float App::GetOutputAspectRatio() const noexcept { return m_impl->GetOutputAspectRatio(); }

void App::Tick() { m_impl->Tick(); }

void App::OnWindowSizeChanged() { m_impl->OnWindowSizeChanged(); }

void App::OnDisplayChanged() { m_impl->OnDisplayChanged(); }

void App::OnResuming() { m_impl->OnResuming(); }

void App::OnSuspending() { m_impl->OnSuspending(); }

void App::OnActivated() { return m_impl->OnActivated(); }

void App::OnDeactivated() { return m_impl->OnDeactivated(); }
