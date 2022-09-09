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

#include "BottomLevelAccelerationStructureGenerator.h"
#include "TopLevelAccelerationStructureGenerator.h"

#include "Camera.h"

#include "MyPhysX.h"

#include "Shaders/Raytracing.hlsl.h"

#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"
#include "ImGuiEx.h"

#include "MyAppData.h"

#include <shellapi.h>

module D3DApp;

import DirectX.Effects.TemporalAntiAliasing;
import DirectX.RaytracingHelpers;
import Material;
import Random;
import SharedData;
import Texture;

using namespace DirectX;
using namespace DirectX::Effects;
using namespace DirectX::RaytracingHelpers;
using namespace DX;
using namespace Microsoft::WRL;
using namespace nv_helpers_dx12;
using namespace PhysicsHelpers;
using namespace physx;
using namespace std;
using namespace std::filesystem;
using namespace WindowHelpers;

struct D3DApp::Impl : IDeviceNotify {
	Impl(const shared_ptr<WindowModeHelper>& windowModeHelper) noexcept(false) : m_windowModeHelper(windowModeHelper) {
		windowModeHelper->SetFullscreenResolutionHandledByWindow(false);

		{
			if (GraphicsSettingsData::Load<GraphicsSettingsData::Raytracing::MaxTraceRecursionDepth>(m_raytracingMaxTraceRecursionDepth)) {
				m_raytracingMaxTraceRecursionDepth = clamp(m_raytracingMaxTraceRecursionDepth, 1u, static_cast<UINT>(D3D12_RAYTRACING_MAX_DECLARABLE_TRACE_RECURSION_DEPTH));
			}

			if (GraphicsSettingsData::Load<GraphicsSettingsData::Raytracing::SamplesPerPixel>(m_raytracingSamplesPerPixel)) {
				m_raytracingSamplesPerPixel = clamp(m_raytracingSamplesPerPixel, 1u, MaxRaytracingSamplesPerPixel);
			}

			{
				ignore = GraphicsSettingsData::Load<GraphicsSettingsData::TemporalAntiAliasing::IsEnabled>(m_isTemporalAntiAliasingEnabled);

				if (GraphicsSettingsData::Load<GraphicsSettingsData::TemporalAntiAliasing::Alpha>(m_temporalAntiAliasingConstant.Alpha)) {
					m_temporalAntiAliasingConstant.Alpha = clamp(m_temporalAntiAliasingConstant.Alpha, 0.0f, 1.0f);
				}

				if (GraphicsSettingsData::Load<GraphicsSettingsData::TemporalAntiAliasing::ColorBoxSigma>(m_temporalAntiAliasingConstant.ColorBoxSigma)) {
					m_temporalAntiAliasingConstant.ColorBoxSigma = clamp(m_temporalAntiAliasingConstant.ColorBoxSigma, 0.0f, MaxTemporalAntiAliasingColorBoxSigma);
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

		m_deviceResources->SetWindow(windowModeHelper->hWnd, windowModeHelper->GetResolution());

		m_deviceResources->CreateDeviceResources();
		CreateDeviceDependentResources();

		m_deviceResources->CreateWindowSizeDependentResources();
		CreateWindowSizeDependentResources();

		m_inputDevices.Mouse->SetWindow(windowModeHelper->hWnd);

		{
			constexpr XMFLOAT3 position{ 0, 0, -15 };
			m_firstPersonCamera.SetPosition(position);
			*reinterpret_cast<Camera*>(m_shaderResources.Camera.Memory()) = {
				.Position = position,
				.ProjectionToWorld = m_firstPersonCamera.GetProjection()
			};
		}
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

		const auto windowMode = m_windowModeHelper->GetMode();
		const auto resolution = m_windowModeHelper->GetResolution();
		DWORD windowedStyle, windowedExStyle;
		m_windowModeHelper->GetWindowedStyles(windowedStyle, windowedExStyle);

		Render();

		const auto newWindowMode = m_windowModeHelper->GetMode();
		const auto newResolution = m_windowModeHelper->GetResolution();
		DWORD newWindowedStyle, newWindowedExStyle;
		m_windowModeHelper->GetWindowedStyles(newWindowedStyle, newWindowedExStyle);

		if (const auto isWindowModeChanged = newWindowMode != windowMode, isResolutionChanged = newResolution != resolution;
			isWindowModeChanged || isResolutionChanged || windowedStyle != newWindowedStyle || windowedExStyle != newWindowedExStyle) {
			ThrowIfFailed(m_windowModeHelper->Apply());

			if (isWindowModeChanged) GraphicsSettingsData::Save<GraphicsSettingsData::WindowMode>(newWindowMode);
			if (isResolutionChanged) GraphicsSettingsData::Save<GraphicsSettingsData::Resolution>(newResolution);
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

		m_renderTextureResources = {};

		for (auto& [_, textures] : m_textures) for (auto& [_, texture] : get<0>(textures)) texture.Resource.Reset();

		m_accelerationStructureBuffers = {};

		m_geometries = {};

		m_shaderResources = {};

		m_temporalAntiAliasingEffect.reset();

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
	using GamepadButtonState = GamePad::ButtonStateTracker::ButtonState;
	using GraphicsSettingsData = MyAppData::Settings::Graphics;
	using Key = Keyboard::Keys;

	struct Objects {
		static constexpr LPCSTR
			Environment = "Environment",
			Earth = "Earth", Moon = "Moon", Star = "Star",
			HarmonicOscillator = "HarmonicOscillator";
	};

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

	struct ResourceDescriptorHeapIndex {
		enum {
			LocalResourceDescriptorHeapIndices,
			Camera,
			GlobalData, LocalData,
			PreviousOutputSRV, CurrentOutputSRV, CurrentOutputUAV, MotionVectorsSRV, MotionVectorsUAV, FinalOutputUAV,
			SphereVertices, SphereIndices,
			EnvironmentCubeMap,
			MoonColorMap, MoonNormalMap,
			EarthColorMap, EarthNormalMap,
			Font,
			Count
		};
	};
	unique_ptr<DescriptorHeap> m_resourceDescriptorHeap;

	ComPtr<ID3D12RootSignature> m_rootSignature;

	ComPtr<ID3D12PipelineState> m_pipelineStateObject;

	static constexpr float MaxTemporalAntiAliasingColorBoxSigma = 2;
	bool m_isTemporalAntiAliasingEnabled = true;
	decltype(TemporalAntiAliasingEffect::Constant) m_temporalAntiAliasingConstant;
	unique_ptr<TemporalAntiAliasingEffect> m_temporalAntiAliasingEffect;

	struct GlobalResourceDescriptorHeapIndices {
		UINT
			LocalResourceDescriptorHeapIndices = ResourceDescriptorHeapIndex::LocalResourceDescriptorHeapIndices,
			Camera = ResourceDescriptorHeapIndex::Camera,
			GlobalData = ResourceDescriptorHeapIndex::GlobalData, LocalData = ResourceDescriptorHeapIndex::LocalData,
			Output = ResourceDescriptorHeapIndex::CurrentOutputUAV,
			EnvironmentCubeMap = UINT_MAX;
		XMUINT2 _padding{};
	};
	struct LocalResourceDescriptorHeapIndices {
		struct {
			UINT Vertices, Indices;
			XMUINT2 _padding;
		} TriangleMesh;
		struct {
			UINT ColorMap, NormalMap;
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
		GraphicsResource
			GlobalResourceDescriptorHeapIndices,
			LocalResourceDescriptorHeapIndices,
			Camera,
			GlobalData, LocalData;
	} m_shaderResources;

	struct {
		struct Sphere : unique_ptr<TriangleMesh<VertexPositionNormalTexture, UINT16>> {
			using unique_ptr::unique_ptr;
			using unique_ptr::operator=;
			static constexpr float Radius = 0.5f;
		} Sphere;
	} m_geometries;

	struct {
		struct { AccelerationStructureBuffers Sphere; } BottomLevel;
		AccelerationStructureBuffers TopLevel;
	} m_accelerationStructureBuffers;

	struct { ComPtr<ID3D12Resource> PreviousOutput, CurrentOutput, MotionVectors, FinalOutput; } m_renderTextureResources;

	TextureDictionary m_textures;

	FirstPersonCamera m_firstPersonCamera;

	struct RenderItem {
		string Name;
		UINT InstanceID = UINT_MAX;
		struct {
			struct { UINT Vertices = UINT_MAX, Indices = UINT_MAX; } TriangleMesh;
		} ResourceDescriptorHeapIndices;
		Material Material;
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
	UINT m_raytracingMaxTraceRecursionDepth = 8, m_raytracingSamplesPerPixel = 2;

	bool m_isMenuOpen{};

	void CreateDeviceDependentResources() {
		const auto device = m_deviceResources->GetD3DDevice();

		m_graphicsMemory = make_unique<decltype(m_graphicsMemory)::element_type>(device);

		CreateDescriptorHeaps();

		CreateRootSignatures();

		CreatePipelineStateObjects();

		CreateEffects();

		CreateShaderResources();

		CreateGeometries();

		CreateAccelerationStructures();

		LoadTextures();
	}

	void CreateWindowSizeDependentResources() {
		const auto device = m_deviceResources->GetD3DDevice();

		const auto outputSize = GetOutputSize();

		{
			const auto CreateResource = [&](auto format, auto& resource, UINT srvDescriptorHeapIndex = UINT_MAX, UINT uavDescriptorHeapIndex = UINT_MAX) {
				const CD3DX12_HEAP_PROPERTIES defaultHeapProperties(D3D12_HEAP_TYPE_DEFAULT);
				const auto tex2DDesc = CD3DX12_RESOURCE_DESC::Tex2D(format, static_cast<UINT64>(outputSize.cx), static_cast<UINT64>(outputSize.cy), 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
				ThrowIfFailed(device->CreateCommittedResource(&defaultHeapProperties, D3D12_HEAP_FLAG_NONE, &tex2DDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&resource)));

				if (srvDescriptorHeapIndex != UINT_MAX) {
					CreateShaderResourceView(device, resource.Get(), m_resourceDescriptorHeap->GetCpuHandle(srvDescriptorHeapIndex));
				}

				if (uavDescriptorHeapIndex != UINT_MAX) {
					const D3D12_UNORDERED_ACCESS_VIEW_DESC unorderedAccessViewDesc{ .ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D };
					device->CreateUnorderedAccessView(resource.Get(), nullptr, &unorderedAccessViewDesc, m_resourceDescriptorHeap->GetCpuHandle(uavDescriptorHeapIndex));
				}
			};

			{
				const auto format = m_deviceResources->GetBackBufferFormat();

				CreateResource(format, m_renderTextureResources.PreviousOutput, ResourceDescriptorHeapIndex::PreviousOutputSRV);
				CreateResource(format, m_renderTextureResources.CurrentOutput, ResourceDescriptorHeapIndex::CurrentOutputSRV, ResourceDescriptorHeapIndex::CurrentOutputUAV);
				CreateResource(format, m_renderTextureResources.FinalOutput, UINT_MAX, ResourceDescriptorHeapIndex::FinalOutputUAV);
			}

			CreateResource(DXGI_FORMAT_R32G32_FLOAT, m_renderTextureResources.MotionVectors, ResourceDescriptorHeapIndex::MotionVectorsSRV, ResourceDescriptorHeapIndex::MotionVectorsUAV);
		}

		m_temporalAntiAliasingEffect->TextureSize = outputSize;

		m_firstPersonCamera.SetLens(XM_PIDIV4, static_cast<float>(outputSize.cx) / static_cast<float>(outputSize.cy), 1e-1f, 1e4f);

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

		Clear();

		const auto commandList = m_deviceResources->GetCommandList();

		PIXBeginEvent(commandList, PIX_COLOR_DEFAULT, L"Render");

		ID3D12DescriptorHeap* const descriptorHeaps[]{ m_resourceDescriptorHeap->Heap() };
		commandList->SetDescriptorHeaps(static_cast<UINT>(size(descriptorHeaps)), descriptorHeaps);

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

		m_deviceResources->WaitForGpu();

		m_graphicsMemory->Commit(m_deviceResources->GetCommandQueue());

		PIXEndEvent();
	}

	void BuildTextures() {
		decltype(m_textures) textures;

		textures.DirectoryPath = path(*__wargv).replace_filename(LR"(Textures\)");

		textures[Objects::Environment] = {
			{
				{
					TextureType::CubeMap,
					Texture{
						.DescriptorHeapIndex = ResourceDescriptorHeapIndex::EnvironmentCubeMap,
						.Path = L"Space.dds"
					}
				}
			},
			XMMatrixRotationRollPitchYaw(XM_PI, XM_PI * 0.2f, 0)
		};

		textures[Objects::Moon] = {
			{
				{
					TextureType::ColorMap,
					Texture{
						.DescriptorHeapIndex = ResourceDescriptorHeapIndex::MoonColorMap,
						.Path = L"Moon.jpg"
					}
				},
				{
					TextureType::NormalMap,
					Texture{
						.DescriptorHeapIndex = ResourceDescriptorHeapIndex::MoonNormalMap,
						.Path = L"Moon_Normal.jpg"
					}
				}
			},
			XMMatrixTranslation(0.5f, 0, 0)
		};

		textures[Objects::Earth] = {
			{
				{
					TextureType::ColorMap,
					Texture{
						.DescriptorHeapIndex = ResourceDescriptorHeapIndex::EarthColorMap,
						.Path = L"Earth.jpg"
					}
				},
				{
					TextureType::NormalMap,
					Texture{
						.DescriptorHeapIndex = ResourceDescriptorHeapIndex::EarthNormalMap,
						.Path = L"Earth_Normal.jpg"
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
			renderItem.InstanceID = static_cast<UINT>(renderItems.size());

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
			for (const auto& [Position, Material] : spheres) {
				RenderItem renderItem;
				renderItem.Material = Material;
				AddRenderItem(renderItem, Position, PxSphereGeometry(0.5f));
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
					for (const auto& [Position, Material] : spheres) {
						if ((position - Position).magnitude() < 1) {
							isOverlapping = true;
							break;
						}
					}
					if (isOverlapping) continue;

					RenderItem renderItem;

					renderItem.Name = Objects::HarmonicOscillator;

					if (const auto randomValue = random.Float();
						randomValue < 0.5f) renderItem.Material.AsLambertian(random.Float4());
					else if (randomValue < 0.75f) renderItem.Material.AsMetal(random.Float4(0.5f), random.Float(0, 0.5f));
					else renderItem.Material.AsDielectric(random.Float4(), 1.5f);

					auto& rigidDynamic = AddRenderItem(renderItem, position, PxSphereGeometry(0.075f));
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
				.Position{ -4, 4, 0 },
				.Radius = 0.25f,
				.OrbitalPeriod = 10,
				.Material = Material().AsMetal({ 0.5f, 0.5f, 0.5f, 1 }, 0.8f)
			}, earth{
				.Name = Objects::Earth,
				.Position{ 0, moon.Position.y, 0 },
				.Radius = 1,
				.RotationPeriod = 15,
				.Mass = UniversalGravitation::CalculateMass((moon.Position - earth.Position).magnitude(), moon.OrbitalPeriod),
				.Material = Material().AsLambertian({ 0.1f, 0.2f, 0.5f, 1 })
			}, star{
				.Name = Objects::Star,
				.Position{ 0, -50.1f, 0 },
				.Radius = 50,
				.Material = Material().AsMetal({ 0.5f, 0.5f, 0.5f, 1 }, 0)
			};
			for (const auto& [Name, Position, Radius, RotationPeriod, OrbitalPeriod, Mass, Material] : { moon, earth, star }) {
				RenderItem renderItem;

				renderItem.Name = Name;

				renderItem.Material = Material;

				if (m_textures.contains(Name)) renderItem.pTextures = &m_textures[Name];

				auto& rigidDynamic = AddRenderItem(renderItem, Position, PxSphereGeometry(Radius));
				if (renderItem.Name == Objects::Moon) {
					const auto x = earth.Position - Position;
					const auto magnitude = x.magnitude();
					const auto normalized = x / magnitude;
					const auto linearSpeed = UniversalGravitation::CalculateFirstCosmicSpeed(earth.Mass, magnitude);
					rigidDynamic.setLinearVelocity(linearSpeed * PxVec3(-normalized.z, 0, normalized.x));
					rigidDynamic.setAngularVelocity({ 0, linearSpeed / magnitude, 0 });
				}
				else if (renderItem.Name == Objects::Earth) {
					rigidDynamic.setAngularVelocity({ 0, PxTwoPi / RotationPeriod, 0 });
					PxRigidBodyExt::setMassAndUpdateInertia(rigidDynamic, &Mass, 1);
				}
				else if (renderItem.Name == Objects::Star) rigidDynamic.setMass(0);

				m_physicsObjects.RigidBodies[Name] = { &rigidDynamic, false };
			}
		}

		m_renderItems = move(renderItems);
	}

	void CreateDescriptorHeaps() {
		const auto device = m_deviceResources->GetD3DDevice();

		m_resourceDescriptorHeap = make_unique<DescriptorHeap>(device, ResourceDescriptorHeapIndex::Count);
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

	void CreateEffects() {
		const auto device = m_deviceResources->GetD3DDevice();

		m_temporalAntiAliasingEffect = make_unique<decltype(m_temporalAntiAliasingEffect)::element_type>(device);
		m_temporalAntiAliasingEffect->Constant = m_temporalAntiAliasingConstant;
		m_temporalAntiAliasingEffect->TextureDescriptors = {
			.PreviousOutputSRV = m_resourceDescriptorHeap->GetGpuHandle(ResourceDescriptorHeapIndex::PreviousOutputSRV),
			.CurrentOutputSRV = m_resourceDescriptorHeap->GetGpuHandle(ResourceDescriptorHeapIndex::CurrentOutputSRV),
			.MotionVectorsSRV = m_resourceDescriptorHeap->GetGpuHandle(ResourceDescriptorHeapIndex::MotionVectorsSRV),
			.FinalOutputUAV = m_resourceDescriptorHeap->GetGpuHandle(ResourceDescriptorHeapIndex::FinalOutputUAV)
		};
	}

	void CreateShaderResources() {
		const auto device = m_deviceResources->GetD3DDevice();

		{
			const auto CreateConstantBufferView = [&](const auto& graphicsResource, UINT descriptorHeapIndex) {
				const D3D12_CONSTANT_BUFFER_VIEW_DESC constantBufferViewDesc{
					.BufferLocation = graphicsResource.GpuAddress(),
					.SizeInBytes = static_cast<UINT>(graphicsResource.Size())
				};
				device->CreateConstantBufferView(&constantBufferViewDesc, m_resourceDescriptorHeap->GetCpuHandle(descriptorHeapIndex));
			};

			m_shaderResources.GlobalResourceDescriptorHeapIndices = m_graphicsMemory->AllocateConstant(GlobalResourceDescriptorHeapIndices{
				.EnvironmentCubeMap = m_textures.contains(Objects::Environment) && get<0>(m_textures[Objects::Environment]).contains(TextureType::CubeMap) ? ResourceDescriptorHeapIndex::EnvironmentCubeMap : UINT_MAX
				});

			{
				m_shaderResources.Camera = m_graphicsMemory->AllocateConstant<Camera>();
				CreateConstantBufferView(m_shaderResources.Camera, ResourceDescriptorHeapIndex::Camera);
			}

			{
				m_shaderResources.GlobalData = m_graphicsMemory->AllocateConstant(GlobalData{
					.RaytracingMaxTraceRecursionDepth = m_raytracingMaxTraceRecursionDepth,
					.RaytracingSamplesPerPixel = m_raytracingSamplesPerPixel,
					.EnvironmentMapTransform = m_textures.contains(Objects::Environment) ? XMMatrixTranspose(get<1>(m_textures[Objects::Environment])) : XMMatrixIdentity()
					});
				CreateConstantBufferView(m_shaderResources.GlobalData, ResourceDescriptorHeapIndex::GlobalData);
			}
		}

		{
			const auto CreateShaderResourceView = [&](const auto& graphicsResource, UINT count, UINT stride, UINT descriptorHeapIndex) {
				const D3D12_SHADER_RESOURCE_VIEW_DESC shaderResourceViewDesc{
					.ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
					.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
					.Buffer{
						.NumElements = count,
						.StructureByteStride = stride
					}
				};
				device->CreateShaderResourceView(graphicsResource.Resource(), &shaderResourceViewDesc, m_resourceDescriptorHeap->GetCpuHandle(descriptorHeapIndex));
			};

			const auto renderItemsSize = static_cast<UINT>(m_renderItems.size());

			m_shaderResources.LocalResourceDescriptorHeapIndices = m_graphicsMemory->Allocate(sizeof(LocalResourceDescriptorHeapIndices) * renderItemsSize);
			CreateShaderResourceView(m_shaderResources.LocalResourceDescriptorHeapIndices, renderItemsSize, sizeof(LocalResourceDescriptorHeapIndices), ResourceDescriptorHeapIndex::LocalResourceDescriptorHeapIndices);

			m_shaderResources.LocalData = m_graphicsMemory->Allocate(sizeof(LocalData) * renderItemsSize);
			CreateShaderResourceView(m_shaderResources.LocalData, renderItemsSize, sizeof(LocalData), ResourceDescriptorHeapIndex::LocalData);

			for (const auto& renderItem : m_renderItems) {
				auto& localResourceDescriptorHeapIndices = *(reinterpret_cast<LocalResourceDescriptorHeapIndices*>(m_shaderResources.LocalResourceDescriptorHeapIndices.Memory()) + renderItem.InstanceID);

				localResourceDescriptorHeapIndices = {
					.TriangleMesh{
						.Vertices = renderItem.ResourceDescriptorHeapIndices.TriangleMesh.Vertices,
						.Indices = renderItem.ResourceDescriptorHeapIndices.TriangleMesh.Indices
					},
					.Textures{
						.ColorMap = UINT_MAX,
						.NormalMap = UINT_MAX
					}
				};

				auto& localData = *(reinterpret_cast<LocalData*>(m_shaderResources.LocalData.Memory()) + renderItem.InstanceID);

				localData.Material = renderItem.Material;

				if (renderItem.pTextures != nullptr) {
					const auto& textures = get<0>(*renderItem.pTextures);
					if (textures.contains(TextureType::ColorMap)) {
						localResourceDescriptorHeapIndices.Textures.ColorMap = textures.at(TextureType::ColorMap).DescriptorHeapIndex;
					}
					if (textures.contains(TextureType::NormalMap)) {
						localResourceDescriptorHeapIndices.Textures.NormalMap = textures.at(TextureType::NormalMap).DescriptorHeapIndex;
					}

					localData.TextureTransform = XMMatrixTranspose(get<1>(*renderItem.pTextures));
				}
			}
		}
	}

	void CreateGeometries() {
		const auto device = m_deviceResources->GetD3DDevice();

		auto& sphere = m_geometries.Sphere;

		GeometricPrimitive::VertexCollection vertices;
		GeometricPrimitive::IndexCollection indices;
		GeometricPrimitive::CreateGeoSphere(vertices, indices, sphere.Radius * 2, 6);

		sphere = make_unique<decay_t<decltype(sphere)>::element_type>(device, vertices, indices, D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE);
		sphere->CreateShaderResourceViews(m_resourceDescriptorHeap->GetCpuHandle(ResourceDescriptorHeapIndex::SphereVertices), m_resourceDescriptorHeap->GetCpuHandle(ResourceDescriptorHeapIndex::SphereIndices));
	}

	void CreateBottomLevelAccelerationStructure(const vector<D3D12_RAYTRACING_GEOMETRY_DESC>& geometryDescs, AccelerationStructureBuffers& buffers) {
		const auto device = m_deviceResources->GetD3DDevice();

		BottomLevelAccelerationStructureGenerator bottomLevelAccelerationStructureGenerator(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE);

		for (const auto& geometryDesc : geometryDescs) bottomLevelAccelerationStructureGenerator.AddGeometry(geometryDesc);

		UINT64 scratchSize, resultSize;
		bottomLevelAccelerationStructureGenerator.ComputeBufferSizes(device, scratchSize, resultSize);

		buffers = AccelerationStructureBuffers(device, scratchSize, resultSize, 0);

		bottomLevelAccelerationStructureGenerator.Generate(m_deviceResources->GetCommandList(), buffers.Scratch.Get(), buffers.Result.Get());
	}

	void CreateTopLevelAccelerationStructure(bool updateOnly) {
		TopLevelAccelerationStructureGenerator topLevelAccelerationStructureGenerator(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE);

		for (UINT i = 0, size = static_cast<UINT>(m_renderItems.size()); i < size; i++) {
			auto& renderItem = m_renderItems[i];

			const auto& shape = *renderItem.Shape;

			PxVec3 scale;
			const auto geometry = shape.getGeometry();
			switch (shape.getGeometryType()) {
			case PxGeometryType::eSPHERE: scale = PxVec3(geometry.sphere().radius / m_geometries.Sphere.Radius); break;
			default: throw;
			}

			if (updateOnly) UpdateRenderItem(renderItem);

			PxMat44 world(PxVec4(1, 1, -1, 1));
			world *= PxShapeExt::getGlobalPose(shape, *shape.getActor());
			world.scale(PxVec4(scale, 1));
			topLevelAccelerationStructureGenerator.AddInstance(m_accelerationStructureBuffers.BottomLevel.Sphere.Result->GetGPUVirtualAddress(), XMLoadFloat4x4(reinterpret_cast<const XMFLOAT4X4*>(world.front())), renderItem.InstanceID, i, ~0, D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_FRONT_COUNTERCLOCKWISE);
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

		const auto Translate = [&](const auto& displacement) {
			if (displacement.x == 0 && displacement.y == 0 && displacement.z == 0) return;

			constexpr auto ToPxVec3 = [](const auto& value) { return PxVec3(value.x, value.y, -value.z); };

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

		const auto Pitch = [&](float angle) {
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

		*reinterpret_cast<Camera*>(m_shaderResources.Camera.Memory()) = {
			.Position = m_firstPersonCamera.GetPosition(),
			.ProjectionToWorld = XMMatrixTranspose(XMMatrixInverse(nullptr, m_firstPersonCamera.GetView() * m_firstPersonCamera.GetProjection()))
		};
	}

	void UpdateGlobalData() {
		auto& globalData = *reinterpret_cast<GlobalData*>(m_shaderResources.GlobalData.Memory());

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
			renderItem.Name == Objects::HarmonicOscillator) {
			const auto k = SimpleHarmonicMotion::Spring::CalculateConstant(mass, Period);
			const PxVec3 x(0, position.y - PositionY, 0);
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

		commandList->SetComputeRootSignature(m_rootSignature.Get());
		commandList->SetComputeRootShaderResourceView(0, m_accelerationStructureBuffers.TopLevel.Result->GetGPUVirtualAddress());
		commandList->SetComputeRootConstantBufferView(1, m_shaderResources.GlobalResourceDescriptorHeapIndices.GpuAddress());

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

			ImGui::Begin("##Menu", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_HorizontalScrollbar);

			if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) ImGui::SetWindowFocus();

			if (ImGui::CollapsingHeader("Graphics Settings")) {
				{
					{
						constexpr LPCSTR WindowModes[]{ "Windowed", "Borderless", "Fullscreen" };
						if (auto windowMode = m_windowModeHelper->GetMode();
							ImGui::Combo("Window Mode", reinterpret_cast<int*>(&windowMode), WindowModes, static_cast<int>(size(WindowModes)))) {
							m_windowModeHelper->SetMode(windowMode);
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
								}
								if (isSelected) ImGui::SetItemDefaultFocus();
							}

							ImGui::EndCombo();
						}
					}
				}

				ImGui::Separator();

				if (ImGui::TreeNodeEx("Raytracing", ImGuiTreeNodeFlags_DefaultOpen)) {
					if (ImGui::SliderInt("Max Trace Recursion Depth", reinterpret_cast<int*>(&m_raytracingMaxTraceRecursionDepth), 1, D3D12_RAYTRACING_MAX_DECLARABLE_TRACE_RECURSION_DEPTH, "%d", ImGuiSliderFlags_NoInput)) {
						reinterpret_cast<GlobalData*>(m_shaderResources.GlobalData.Memory())->RaytracingMaxTraceRecursionDepth = m_raytracingMaxTraceRecursionDepth;

						GraphicsSettingsData::Save<GraphicsSettingsData::Raytracing::MaxTraceRecursionDepth>(m_raytracingMaxTraceRecursionDepth);
					}

					if (ImGui::SliderInt("Samples Per Pixel", reinterpret_cast<int*>(&m_raytracingSamplesPerPixel), 1, static_cast<int>(MaxRaytracingSamplesPerPixel), "%d", ImGuiSliderFlags_NoInput)) {
						reinterpret_cast<GlobalData*>(m_shaderResources.GlobalData.Memory())->RaytracingSamplesPerPixel = m_raytracingSamplesPerPixel;

						GraphicsSettingsData::Save<GraphicsSettingsData::Raytracing::SamplesPerPixel>(m_raytracingSamplesPerPixel);
					}

					ImGui::TreePop();
				}

				if (ImGui::TreeNodeEx("Temporal Anti-Aliasing", ImGuiTreeNodeFlags_DefaultOpen)) {
					if (ImGui::Checkbox("Enable", &m_isTemporalAntiAliasingEnabled)) {
						GraphicsSettingsData::Save<GraphicsSettingsData::TemporalAntiAliasing::IsEnabled>(m_isTemporalAntiAliasingEnabled);
					}

					if (ImGui::SliderFloat("Alpha", &m_temporalAntiAliasingConstant.Alpha, 0, 1, "%.3f", ImGuiSliderFlags_NoInput)) {
						m_temporalAntiAliasingEffect->Constant.Alpha = m_temporalAntiAliasingConstant.Alpha;

						GraphicsSettingsData::Save<GraphicsSettingsData::TemporalAntiAliasing::Alpha>(m_temporalAntiAliasingConstant.Alpha);
					}

					if (ImGui::SliderFloat("Color-Box Sigma", &m_temporalAntiAliasingConstant.ColorBoxSigma, 0, MaxTemporalAntiAliasingColorBoxSigma, "%.3f", ImGuiSliderFlags_NoInput)) {
						m_temporalAntiAliasingEffect->Constant.ColorBoxSigma = m_temporalAntiAliasingConstant.ColorBoxSigma;

						GraphicsSettingsData::Save<GraphicsSettingsData::TemporalAntiAliasing::ColorBoxSigma>(m_temporalAntiAliasingConstant.ColorBoxSigma);
					}

					ImGui::TreePop();
				}
			}

			if (ImGui::CollapsingHeader("Controls")) {
				const auto AddContents = [](auto treeLabel, auto tableID, const initializer_list<pair<LPCSTR, LPCSTR>>& list) {
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
