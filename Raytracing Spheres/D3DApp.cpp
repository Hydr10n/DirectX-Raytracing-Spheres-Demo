module;

#include "pch.h"

#include "DeviceResources.h"

#include "StepTimer.h"

#include "directxtk12/GraphicsMemory.h"

#include "directxtk12/GamePad.h"
#include "directxtk12/Keyboard.h"
#include "directxtk12/Mouse.h"

#include "directxtk12/DirectXHelpers.h"

#include "directxtk12/DescriptorHeap.h"

#include "directxtk12/GeometricPrimitive.h"

#include "Camera.h"

#include "MyPhysX.h"

#include "Shaders/Raytracing.hlsl.h"

#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"
#include "ImGuiEx.h"

#include "MyAppData.h"

#include <span>

#include <map>

#include <shellapi.h>

module D3DApp;

import DirectX.BufferHelpers;
import DirectX.PostProcess.TemporalAntiAliasing;
import DirectX.RaytracingHelpers;
import Material;
import Random;
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
}

struct D3DApp::Impl : IDeviceNotify {
	Impl(const shared_ptr<WindowModeHelper>& windowModeHelper) noexcept(false) : m_windowModeHelper(windowModeHelper) {
		windowModeHelper->SetFullscreenResolutionHandledByWindow(false);

		{
			GraphicsSettings.Raytracing.MaxTraceRecursionDepth = clamp(GraphicsSettings.Raytracing.MaxTraceRecursionDepth, 1u, static_cast<UINT>(D3D12_RAYTRACING_MAX_DECLARABLE_TRACE_RECURSION_DEPTH));
			GraphicsSettings.Raytracing.SamplesPerPixel = clamp(GraphicsSettings.Raytracing.SamplesPerPixel, 1u, MaxRaytracingSamplesPerPixel);

			GraphicsSettings.TemporalAntiAliasing.Alpha = clamp(GraphicsSettings.TemporalAntiAliasing.Alpha, 0.0f, 1.0f);
			GraphicsSettings.TemporalAntiAliasing.ColorBoxSigma = clamp(GraphicsSettings.TemporalAntiAliasing.ColorBoxSigma, 0.0f, MaxTemporalAntiAliasingColorBoxSigma);
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

		m_firstPersonCamera.SetPosition({ 0, 0, -15 });

		BuildTextures();

		BuildRenderItems();

		m_deviceResources->RegisterDeviceNotify(this);

		m_deviceResources->SetWindow(windowModeHelper->hWnd, windowModeHelper->GetResolution());

		m_deviceResources->CreateDeviceResources();
		CreateDeviceDependentResources();

		m_deviceResources->CreateWindowSizeDependentResources();
		CreateWindowSizeDependentResources();

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

	void Tick() {
		m_stepTimer.Tick([&] { Update(); });

		Render();

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

		m_topLevelAccelerationStructure = {};
		m_bottomLevelAccelerationStructures = {};

		m_triangleMeshes = {};

		m_shaderBuffers = {};

		m_temporalAntiAliasing.reset();

		m_pipelineStateObject.Reset();

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

	unique_ptr<DeviceResources> m_deviceResources = make_unique<decltype(m_deviceResources)::element_type>(D3D12_RAYTRACING_TIER_1_1);

	StepTimer m_stepTimer;

	unique_ptr<GraphicsMemory> m_graphicsMemory;

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

	struct ObjectNames {
		static constexpr LPCSTR
			Environment = "Environment",
			Sphere = "Sphere",
			Earth = "Earth", Moon = "Moon", Star = "Star",
			HarmonicOscillator = "HarmonicOscillator";
	};

	struct ResourceDescriptorHeapIndex {
		enum {
			LocalResourceDescriptorHeapIndices,
			Camera,
			GlobalData, LocalData,
			PreviousOutputSRV, CurrentOutputSRV, CurrentOutputUAV, MotionVectorsSRV, MotionVectorsUAV, FinalOutputUAV,
			SphereVertices, SphereIndices,
			EnvironmentCubeMap,
			MoonBaseColorMap, MoonNormalMap,
			EarthBaseColorMap, EarthNormalMap,
			Font,
			Count
		};
	};
	unique_ptr<DescriptorPile> m_resourceDescriptorHeap;

	ComPtr<ID3D12RootSignature> m_rootSignature;

	ComPtr<ID3D12PipelineState> m_pipelineStateObject;

	static constexpr float MaxTemporalAntiAliasingColorBoxSigma = 2;
	unique_ptr<TemporalAntiAliasing> m_temporalAntiAliasing;

	struct GlobalResourceDescriptorHeapIndices {
		UINT
			LocalResourceDescriptorHeapIndices = ~0u,
			Camera = ~0u,
			GlobalData = ~0u, LocalData = ~0u,
			Output = ~0u,
			EnvironmentCubeMap = ~0u;
		XMUINT2 _padding;
	};
	struct LocalResourceDescriptorHeapIndices {
		struct {
			UINT Vertices = ~0u, Indices = ~0u;
			XMUINT2 _padding;
		} TriangleMesh;
		struct {
			UINT BaseColorMap = ~0u, NormalMap = ~0u;
			XMUINT2 _padding;
		} Textures;
	};
	struct GlobalData {
		UINT RaytracingMaxTraceRecursionDepth, RaytracingSamplesPerPixel, FrameCount;
		UINT _padding;
		XMMATRIX EnvironmentMapTransform;
	};
	struct LocalData {
		XMMATRIX TextureTransform;
		Material Material;
	};
	struct {
		unique_ptr<ConstantBuffer<GlobalResourceDescriptorHeapIndices>> GlobalResourceDescriptorHeapIndices;
		unique_ptr<ConstantBuffer<Camera>> Camera;
		unique_ptr<ConstantBuffer<GlobalData>> GlobalData;
		unique_ptr<StructuredBuffer<LocalResourceDescriptorHeapIndices>> LocalResourceDescriptorHeapIndices;
		unique_ptr<StructuredBuffer<LocalData>> LocalData;
	} m_shaderBuffers;

	map<string, shared_ptr<TriangleMesh<VertexPositionNormalTexture, UINT16>>, less<>> m_triangleMeshes;

	map<string, BottomLevelAccelerationStructure, less<>> m_bottomLevelAccelerationStructures;
	TopLevelAccelerationStructure m_topLevelAccelerationStructure;

	struct { Texture PreviousOutput, CurrentOutput, MotionVectors, FinalOutput; } m_renderTextures;

	TextureDictionary m_textures;

	FirstPersonCamera m_firstPersonCamera;

	struct RenderItem {
		string Name;
		UINT InstanceID = ~0u;
		struct {
			struct { UINT Vertices = ~0u, Indices = ~0u; } TriangleMesh;
		} ResourceDescriptorHeapIndices;
		Material Material{};
		TextureDictionary::mapped_type* pTextures{};
		PxShape* Shape{};
	};
	vector<RenderItem> m_renderItems;

	bool m_isPhysicsSimulationRunning = true;
	struct {
		map<string, tuple<PxRigidBody*, bool /*IsGravityEnabled*/>, less<>> RigidBodies;
		const struct { PxReal PositionY = 0.5f, Period = 3; } Spring;
	} m_physicsObjects;
	MyPhysX m_myPhysX;

	static constexpr UINT MaxRaytracingSamplesPerPixel = 16;

	bool m_isMenuOpen = UISettings.Menu.IsOpenOnStartup;

	bool m_isWindowSettingChanged{};

	void CreateDeviceDependentResources() {
		const auto device = m_deviceResources->GetD3DDevice();

		m_graphicsMemory = make_unique<decltype(m_graphicsMemory)::element_type>(device);

		CreateDescriptorHeaps();

		CreateRootSignatures();

		CreatePipelineStateObjects();

		CreatePostProcess();

		CreateShaderBuffers();

		CreateGeometries();

		CreateAccelerationStructures();

		LoadTextures();
	}

	void CreateWindowSizeDependentResources() {
		const auto device = m_deviceResources->GetD3DDevice();

		const auto outputSize = GetOutputSize();

		{
			const auto CreateResource = [&](DXGI_FORMAT format, Texture& texture, UINT srvDescriptorHeapIndex = ~0u, UINT uavDescriptorHeapIndex = ~0u) {
				const CD3DX12_HEAP_PROPERTIES defaultHeapProperties(D3D12_HEAP_TYPE_DEFAULT);
				const auto tex2DDesc = CD3DX12_RESOURCE_DESC::Tex2D(format, static_cast<UINT64>(outputSize.cx), static_cast<UINT64>(outputSize.cy), 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
				ThrowIfFailed(device->CreateCommittedResource(&defaultHeapProperties, D3D12_HEAP_FLAG_NONE, &tex2DDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&texture.Resource)));

				if (srvDescriptorHeapIndex != ~0u) {
					device->CreateShaderResourceView(texture.Resource.Get(), nullptr, m_resourceDescriptorHeap->GetCpuHandle(srvDescriptorHeapIndex));
					texture.DescriptorHeapIndices.SRV = srvDescriptorHeapIndex;
				}
				if (uavDescriptorHeapIndex != ~0u) {
					device->CreateUnorderedAccessView(texture.Resource.Get(), nullptr, nullptr, m_resourceDescriptorHeap->GetCpuHandle(uavDescriptorHeapIndex));
					texture.DescriptorHeapIndices.UAV = uavDescriptorHeapIndex;
				}
			};

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
			m_firstPersonCamera.SetLens(XM_PIDIV4, static_cast<float>(outputSize.cx) / static_cast<float>(outputSize.cy), 1e-1f, 1e4f);
			m_shaderBuffers.Camera->GetData().ProjectionToWorld = XMMatrixTranspose(XMMatrixInverse(nullptr, m_firstPersonCamera.GetView() * m_firstPersonCamera.GetProjection()));
		}

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

		UpdateGlobalData();

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

		const auto commandList = m_deviceResources->GetCommandList();

		Clear();

		PIXBeginEvent(commandList, PIX_COLOR_DEFAULT, L"Render");

		ID3D12DescriptorHeap* const descriptorHeaps[]{ m_resourceDescriptorHeap->Heap() };
		commandList->SetDescriptorHeaps(static_cast<UINT>(size(descriptorHeaps)), descriptorHeaps);

		{
			DispatchRays();

			if (GraphicsSettings.TemporalAntiAliasing.IsEnabled) {
				const ScopedBarrier scopedBarrier(
					commandList,
					{
						CD3DX12_RESOURCE_BARRIER::Transition(m_renderTextures.PreviousOutput.Resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
						CD3DX12_RESOURCE_BARRIER::Transition(m_renderTextures.CurrentOutput.Resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
						CD3DX12_RESOURCE_BARRIER::Transition(m_renderTextures.MotionVectors.Resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
					}
				);

				m_temporalAntiAliasing->Process(commandList);
			}

			{
				const auto
					renderTarget = m_deviceResources->GetRenderTarget(),
					previousOutput = m_renderTextures.PreviousOutput.Resource.Get(),
					output = (GraphicsSettings.TemporalAntiAliasing.IsEnabled ? m_renderTextures.FinalOutput : m_renderTextures.CurrentOutput).Resource.Get();

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

		m_deviceResources->WaitForGpu();

		m_graphicsMemory->Commit(m_deviceResources->GetCommandQueue());

		PIXEndEvent();
	}

	void BuildTextures() {
		decltype(m_textures) textures;

		textures.DirectoryPath = path(*__wargv).replace_filename(L"Textures");

		textures[ObjectNames::Environment] = {
			{
				{
					TextureType::CubeMap,
					Texture{
						.DescriptorHeapIndices{
							.SRV = ResourceDescriptorHeapIndex::EnvironmentCubeMap
						},
						.FilePath = L"Space.dds"
					}
				}
			},
			XMMatrixRotationRollPitchYaw(XM_PI, XM_PI * 0.2f, 0)
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
		constexpr auto threadCount = 8;

		m_textures.Load(device, commandQueue, *m_resourceDescriptorHeap, threadCount);
	}

	void BuildRenderItems() {
		decltype(m_renderItems) renderItems;

		const auto& material = *m_myPhysX.GetPhysics().createMaterial(0.5f, 0.5f, 0.6f);

		const auto AddRenderItem = [&](RenderItem& renderItem, const auto& transform, const PxSphereGeometry& geometry) -> decltype(auto) {
			renderItem.InstanceID = static_cast<UINT>(size(renderItems));

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
				PxVec3 Position;
				Material Material;
			} objects[]{
				{
					{ -2, 0.5f, 0 },
					Material::Lambertian({ 0.1f, 0.2f, 0.5f, 1 })
				},
				{
					{ 0, 0.5f, 0 },
					Material::Dielectric({ 1, 1, 1, 1 }, 1.5f)
				},
				{
					{ 2, 0.5f, 0 },
					Material::Metal({ 0.7f, 0.6f, 0.5f, 1 }, 0.2f)
				}
			};
			for (const auto& [Position, Material] : objects) {
				RenderItem renderItem;
				renderItem.Material = Material;
				AddRenderItem(renderItem, Position, PxSphereGeometry(0.5f));
			}

			Random random;
			for (int i = -10; i < 10; i++) {
				for (int j = -10; j < 10; j++) {
					constexpr auto A = 0.5f;
					const auto omega = PxTwoPi / m_physicsObjects.Spring.Period;

					PxVec3 position;
					position.x = static_cast<float>(i) + 0.7f * random.Float();
					position.y = m_physicsObjects.Spring.PositionY + SimpleHarmonicMotion::Spring::CalculateDisplacement(A, omega, 0.0f, position.x);
					position.z = static_cast<float>(j) - 0.7f * random.Float();

					bool isOverlapping = false;
					for (const auto& [Position, Material] : objects) {
						if ((position - Position).magnitude() < 1) {
							isOverlapping = true;
							break;
						}
					}
					if (isOverlapping) continue;

					RenderItem renderItem;

					renderItem.Name = ObjectNames::HarmonicOscillator;

					if (const auto randomValue = random.Float();
						randomValue < 0.5f) renderItem.Material = Material::Lambertian(random.Float4());
					else if (randomValue < 0.75f) renderItem.Material = Material::Metal(random.Float4(0.5f), random.Float(0, 0.5f));
					else renderItem.Material = Material::Dielectric(random.Float4(), 1.5f);

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
				.Material = Material::Metal({ 0.5f, 0.5f, 0.5f, 1 }, 0.8f)
			}, earth{
				.Name = ObjectNames::Earth,
				.Position{ 0, moon.Position.y, 0 },
				.Radius = 1,
				.RotationPeriod = 15,
				.Mass = UniversalGravitation::CalculateMass((moon.Position - earth.Position).magnitude(), moon.OrbitalPeriod),
				.Material = Material::Lambertian({ 0.1f, 0.2f, 0.5f, 1 })
			}, star{
				.Name = ObjectNames::Star,
				.Position{ 0, -50.1f, 0 },
				.Radius = 50,
				.Material = Material::Metal({ 0.5f, 0.5f, 0.5f, 1 }, 0)
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

		m_resourceDescriptorHeap = make_unique<decltype(m_resourceDescriptorHeap)::element_type>(device, ResourceDescriptorHeapIndex::Count);
	}

	void CreateRootSignatures() {
		const auto device = m_deviceResources->GetD3DDevice();

		ThrowIfFailed(device->CreateRootSignature(0, g_pRaytracing, size(g_pRaytracing), IID_PPV_ARGS(&m_rootSignature)));
	}

	void CreatePipelineStateObjects() {
		const auto device = m_deviceResources->GetD3DDevice();

		const CD3DX12_SHADER_BYTECODE shaderByteCode(g_pRaytracing, size(g_pRaytracing));
		const D3D12_COMPUTE_PIPELINE_STATE_DESC computePipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS = shaderByteCode };
		ThrowIfFailed(device->CreateComputePipelineState(&computePipelineStateDesc, IID_PPV_ARGS(&m_pipelineStateObject)));
	}

	void CreatePostProcess() {
		const auto device = m_deviceResources->GetD3DDevice();

		m_temporalAntiAliasing = make_unique<decltype(m_temporalAntiAliasing)::element_type>(device);
		m_temporalAntiAliasing->Constant = GraphicsSettings.TemporalAntiAliasing;
		m_temporalAntiAliasing->TextureDescriptors = {
			.PreviousOutputSRV = m_resourceDescriptorHeap->GetGpuHandle(ResourceDescriptorHeapIndex::PreviousOutputSRV),
			.CurrentOutputSRV = m_resourceDescriptorHeap->GetGpuHandle(ResourceDescriptorHeapIndex::CurrentOutputSRV),
			.MotionVectorsSRV = m_resourceDescriptorHeap->GetGpuHandle(ResourceDescriptorHeapIndex::MotionVectorsSRV),
			.FinalOutputUAV = m_resourceDescriptorHeap->GetGpuHandle(ResourceDescriptorHeapIndex::FinalOutputUAV)
		};
	}

	void CreateShaderBuffers() {
		const auto device = m_deviceResources->GetD3DDevice();

		{
			const auto CreateBuffer = [&](auto& uploadBuffer, const auto& data, UINT descriptorHeapIndex = ~0u) {
				uploadBuffer = make_unique<decay_t<decltype(uploadBuffer)>::element_type>(device);
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
					.EnvironmentCubeMap = m_textures.contains(ObjectNames::Environment) && get<0>(m_textures.at(ObjectNames::Environment)).contains(TextureType::CubeMap) ? ResourceDescriptorHeapIndex::EnvironmentCubeMap : ~0u
				}
			);

			CreateBuffer(
				m_shaderBuffers.Camera,
				Camera{
					.Position = m_firstPersonCamera.GetPosition()
				},
				ResourceDescriptorHeapIndex::Camera
			);

			CreateBuffer(
				m_shaderBuffers.GlobalData,
				GlobalData{
					.RaytracingMaxTraceRecursionDepth = GraphicsSettings.Raytracing.MaxTraceRecursionDepth,
					.RaytracingSamplesPerPixel = GraphicsSettings.Raytracing.SamplesPerPixel,
					.EnvironmentMapTransform = m_textures.contains(ObjectNames::Environment) ? XMMatrixTranspose(get<1>(m_textures.at(ObjectNames::Environment))) : XMMatrixIdentity()
				},
				ResourceDescriptorHeapIndex::GlobalData
			);
		}

		{
			const auto CreateBuffer = [&](auto& uploadBuffer, size_t count, UINT descriptorHeapIndex) {
				uploadBuffer = make_unique<decay_t<decltype(uploadBuffer)>::element_type>(device, count);
				uploadBuffer->CreateShaderResourceView(m_resourceDescriptorHeap->GetCpuHandle(descriptorHeapIndex));
			};

			const auto renderItemsSize = static_cast<UINT>(size(m_renderItems));

			CreateBuffer(m_shaderBuffers.LocalResourceDescriptorHeapIndices, renderItemsSize, ResourceDescriptorHeapIndex::LocalResourceDescriptorHeapIndices);

			CreateBuffer(m_shaderBuffers.LocalData, renderItemsSize, ResourceDescriptorHeapIndex::LocalData);

			for (const auto& renderItem : m_renderItems) {
				auto& localResourceDescriptorHeapIndices = (*m_shaderBuffers.LocalResourceDescriptorHeapIndices)[renderItem.InstanceID];

				localResourceDescriptorHeapIndices = {
					.TriangleMesh{
						.Vertices = renderItem.ResourceDescriptorHeapIndices.TriangleMesh.Vertices,
						.Indices = renderItem.ResourceDescriptorHeapIndices.TriangleMesh.Indices
					}
				};

				auto& localData = (*m_shaderBuffers.LocalData)[renderItem.InstanceID];

				localData.Material = renderItem.Material;

				if (renderItem.pTextures != nullptr) {
					for (const auto& [TextureType, Texture] : get<0>(*renderItem.pTextures)) {
						auto& textures = localResourceDescriptorHeapIndices.Textures;
						UINT* p;
						switch (TextureType) {
						case TextureType::BaseColorMap: p = &textures.BaseColorMap; break;
						case TextureType::NormalMap: p = &textures.NormalMap; break;
						default: p = nullptr; break;
						}
						if (p != nullptr) *p = Texture.DescriptorHeapIndices.SRV;
					}

					localData.TextureTransform = XMMatrixTranspose(get<1>(*renderItem.pTextures));
				}
			}
		}
	}

	void CreateGeometries() {
		const auto device = m_deviceResources->GetD3DDevice();

		auto& sphere = m_triangleMeshes[ObjectNames::Sphere];

		GeometricPrimitive::VertexCollection vertices;
		GeometricPrimitive::IndexCollection indices;
		GeometricPrimitive::CreateGeoSphere(vertices, indices, 1, 6);

		sphere = make_shared<decay_t<decltype(sphere)>::element_type>(device, vertices, indices, D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE);
		sphere->CreateShaderResourceViews(m_resourceDescriptorHeap->GetCpuHandle(ResourceDescriptorHeapIndex::SphereVertices), m_resourceDescriptorHeap->GetCpuHandle(ResourceDescriptorHeapIndex::SphereIndices));
	}

	void CreateBottomLevelAccelerationStructure(span<const D3D12_RAYTRACING_GEOMETRY_DESC> geometryDescs, BottomLevelAccelerationStructure& accelerationStructure) {
		accelerationStructure.Build(m_deviceResources->GetD3DDevice(), m_deviceResources->GetCommandList(), geometryDescs, false);
	}

	void CreateTopLevelAccelerationStructure(bool updateOnly) {
		const auto renderItemsSize = static_cast<UINT>(size(m_renderItems));

		vector<D3D12_RAYTRACING_INSTANCE_DESC> instanceDescs;
		instanceDescs.reserve(renderItemsSize);

		for (UINT i = 0; i < renderItemsSize; i++) {
			auto& renderItem = m_renderItems[i];

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
			instanceDesc.InstanceID = renderItem.InstanceID;
			instanceDesc.InstanceMask = ~0u;
			instanceDesc.InstanceContributionToHitGroupIndex = i;
			instanceDesc.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_FRONT_COUNTERCLOCKWISE;
			instanceDesc.AccelerationStructure = m_bottomLevelAccelerationStructures.at(object.Name).GetBuffer()->GetGPUVirtualAddress();
			instanceDescs.emplace_back(instanceDesc);
		}

		m_topLevelAccelerationStructure.Build(m_deviceResources->GetD3DDevice(), m_deviceResources->GetCommandList(), instanceDescs, updateOnly);
	}

	void CreateAccelerationStructures() {
		const auto commandList = m_deviceResources->GetCommandList();

		ThrowIfFailed(commandList->Reset(m_deviceResources->GetCommandAllocator(), nullptr));

		CreateBottomLevelAccelerationStructure(initializer_list{ m_triangleMeshes.at(ObjectNames::Sphere)->GetGeometryDesc() }, m_bottomLevelAccelerationStructures[ObjectNames::Sphere] = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE);

		m_topLevelAccelerationStructure = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
		CreateTopLevelAccelerationStructure(false);

		ThrowIfFailed(commandList->Close());

		m_deviceResources->GetCommandQueue()->ExecuteCommandLists(1, CommandListCast(&commandList));

		m_deviceResources->WaitForGpu();
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
				if (gamepadStateTracker.view == GamepadButtonState::PRESSED) m_isPhysicsSimulationRunning = !m_isPhysicsSimulationRunning;
				if (keyboardStateTracker.IsKeyPressed(Key::Tab)) m_isPhysicsSimulationRunning = !m_isPhysicsSimulationRunning;
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

		const auto Translate = [&](const XMFLOAT3& displacement) {
			if (displacement.x == 0 && displacement.y == 0 && displacement.z == 0) return;

			constexpr auto ToPxVec3 = [](const XMFLOAT3& value) { return PxVec3(value.x, value.y, -value.z); };

			const auto& [Right, Up, Forward] = m_firstPersonCamera.GetDirections();
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
			m_firstPersonCamera.Yaw(angle);
		};

		const auto Pitch = [&](float angle) {
			if (angle == 0) return;
			if (const auto pitch = asin(m_firstPersonCamera.GetDirections().Forward.y);
				pitch - angle > XM_PIDIV2) angle = -max(0.0f, XM_PIDIV2 - pitch - 0.1f);
			else if (pitch - angle < -XM_PIDIV2) angle = -min(0.0f, XM_PIDIV2 + pitch + 0.1f);
			m_firstPersonCamera.Pitch(angle);
		};

		if (gamepadState.IsConnected()) {
			const auto translationSpeed = elapsedSeconds * 15 * (gamepadState.IsLeftTriggerPressed() ? 0.5f : 1) * (gamepadState.IsRightTriggerPressed() ? 2.0f : 1);
			const XMFLOAT3 displacement{ gamepadState.thumbSticks.leftX * translationSpeed, 0, gamepadState.thumbSticks.leftY * translationSpeed };
			Translate(displacement);

			const auto rotationSpeed = elapsedSeconds * XM_2PI * 0.4f;
			Yaw(gamepadState.thumbSticks.rightX * rotationSpeed);
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
			Yaw(XMConvertToRadians(static_cast<float>(mouseState.x)) * rotationSpeed);
			Pitch(XMConvertToRadians(static_cast<float>(mouseState.y)) * rotationSpeed);
		}

		m_shaderBuffers.Camera->GetData() = {
			.Position = m_firstPersonCamera.GetPosition(),
			.ProjectionToWorld = XMMatrixTranspose(XMMatrixInverse(nullptr, m_firstPersonCamera.GetView() * m_firstPersonCamera.GetProjection()))
		};
	}

	void UpdateGlobalData() {
		auto& globalData = m_shaderBuffers.GlobalData->GetData();

		globalData.FrameCount = m_stepTimer.GetFrameCount();
	}

	void UpdateRenderItem(RenderItem& renderItem) const {
		const auto& shape = *renderItem.Shape;

		const auto rigidBody = renderItem.Shape->getActor()->is<PxRigidBody>();
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
		if (m_isPhysicsSimulationRunning) CreateTopLevelAccelerationStructure(true);

		const auto commandList = m_deviceResources->GetCommandList();

		commandList->SetComputeRootSignature(m_rootSignature.Get());
		commandList->SetComputeRootShaderResourceView(0, m_topLevelAccelerationStructure.GetBuffer()->GetGPUVirtualAddress());
		commandList->SetComputeRootConstantBufferView(1, m_shaderBuffers.GlobalResourceDescriptorHeapIndices->GetResource()->GetGPUVirtualAddress());

		commandList->SetPipelineState(m_pipelineStateObject.Get());

		const auto outputSize = m_deviceResources->GetOutputSize();
		commandList->Dispatch(static_cast<UINT>((outputSize.cx + 15) / 16), static_cast<UINT>((outputSize.cy + 15) / 16), 1);
	}

	void RenderMenu() {
		ImGui_ImplDX12_NewFrame();
		ImGui_ImplWin32_NewFrame();

		{
			const auto& displayResolution = *--cend(g_displayResolutions);
			ImGui::GetIO().DisplaySize = { static_cast<float>(displayResolution.cx), static_cast<float>(displayResolution.cy) };
		}

		ImGui::NewFrame();

		{
			ImGui::SetNextWindowPos({});
			ImGui::SetNextWindowSize({ static_cast<float>(GetOutputSize().cx), 0 });
			ImGui::SetNextWindowBgAlpha(UISettings.Menu.BackgroundOpacity);

			ImGui::Begin("Menu", &m_isMenuOpen, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_HorizontalScrollbar);

			if (ImGui::CollapsingHeader("Settings")) {
				if (ImGui::TreeNode("Graphics")) {
					bool isChanged = false;

					{
						constexpr LPCSTR WindowModes[]{ "Windowed", "Borderless", "Fullscreen" };
						if (auto windowMode = m_windowModeHelper->GetMode();
							ImGui::Combo("Window Mode", reinterpret_cast<int*>(&windowMode), WindowModes, static_cast<int>(size(WindowModes)))) {
							m_windowModeHelper->SetMode(windowMode);

							GraphicsSettings.WindowMode = windowMode;
							m_isWindowSettingChanged = isChanged = true;
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

					if (ImGui::TreeNodeEx("Raytracing", ImGuiTreeNodeFlags_DefaultOpen)) {
						auto& raytracingSettings = GraphicsSettings.Raytracing;

						auto& globalData = m_shaderBuffers.GlobalData->GetData();

						if (ImGui::SliderInt("Max Trace Recursion Depth", reinterpret_cast<int*>(&raytracingSettings.MaxTraceRecursionDepth), 1, D3D12_RAYTRACING_MAX_DECLARABLE_TRACE_RECURSION_DEPTH, "%d", ImGuiSliderFlags_NoInput)) {
							globalData.RaytracingMaxTraceRecursionDepth = raytracingSettings.MaxTraceRecursionDepth;

							isChanged = true;
						}

						if (ImGui::SliderInt("Samples Per Pixel", reinterpret_cast<int*>(&raytracingSettings.SamplesPerPixel), 1, static_cast<int>(MaxRaytracingSamplesPerPixel), "%d", ImGuiSliderFlags_NoInput)) {
							globalData.RaytracingSamplesPerPixel = raytracingSettings.SamplesPerPixel;

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

						if (ImGui::SliderFloat("Color-Box Sigma", &temporalAntiAliasingSettings.ColorBoxSigma, 0, MaxTemporalAntiAliasingColorBoxSigma, "%.2f", ImGuiSliderFlags_NoInput)) {
							m_temporalAntiAliasing->Constant.ColorBoxSigma = temporalAntiAliasingSettings.ColorBoxSigma;

							isChanged = true;
						}

						ImGui::TreePop();
					}

					ImGui::TreePop();

					if (isChanged) GraphicsSettings.Save();
				}

				if (ImGui::TreeNode("UI")) {
					bool isChanged = false;

					if (ImGui::TreeNodeEx("Menu", ImGuiTreeNodeFlags_DefaultOpen)) {
						auto& menuSettings = UISettings.Menu;

						if (ImGui::Checkbox("Open on Startup", &menuSettings.IsOpenOnStartup)) isChanged = true;

						if (ImGui::SliderFloat("Background Opacity", &menuSettings.BackgroundOpacity, 0, 1, "%.2f", ImGuiSliderFlags_NoInput)) isChanged = true;

						ImGui::TreePop();
					}

					ImGui::TreePop();

					if (isChanged) UISettings.Save();
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
					"Mouse",
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
