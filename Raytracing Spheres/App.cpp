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

#include "directxtk12/PostProcess.h"

#include "directxtk12/GeometricPrimitive.h"

#include "NRD.h"

#include "PhysX.h"

#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"
#include "ImGuiEx.h"

#include "MyAppData.h"

#include <shellapi.h>

#include "Shaders/Raytracing.hlsl.h"

module App;

import Camera;
import DirectX.BufferHelpers;
import DirectX.PostProcess.DenoisedComposition;
import DirectX.PostProcess.TemporalAntiAliasing;
import DirectX.PostProcess.TextureAlphaBlend;
import DirectX.RaytracingHelpers;
import HaltonSamplePattern;
import Material;
import NRD;
import Random;
import RaytracingShaderData;
import SharedData;
import Texture;

using namespace DirectX;
using namespace DirectX::BufferHelpers;
using namespace DirectX::PostProcess;
using namespace DirectX::RaytracingHelpers;
using namespace DirectX::SimpleMath;
using namespace DX;
using namespace Microsoft::WRL;
using namespace nrd;
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

#define MAKE_NAME(name) static constexpr LPCSTR name = #name;

struct App::Impl : IDeviceNotify {
	Impl(const shared_ptr<WindowModeHelper>& windowModeHelper) noexcept(false) : m_windowModeHelper(windowModeHelper) {
		CheckSettings();

		{
			BuildTextures();

			BuildRenderObjects();
		}

		{
			ImGui::CreateContext();

			ImGui::StyleColorsDark();

			auto& IO = ImGui::GetIO();

			IO.IniFilename = IO.LogFilename = nullptr;

			IO.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
			IO.BackendFlags |= ImGuiBackendFlags_HasGamepad;

			ImGui_ImplWin32_Init(m_windowModeHelper->hWnd);

			m_UIStates.IsVisible = UISettings.ShowOnStartup;
		}

		{
			m_deviceResources->RegisterDeviceNotify(this);

			m_deviceResources->SetWindow(windowModeHelper->hWnd, windowModeHelper->GetResolution());

			m_deviceResources->EnableVSync(GraphicsSettings.IsVSyncEnabled);

			m_deviceResources->CreateDeviceResources();
			CreateDeviceDependentResources();

			m_deviceResources->CreateWindowSizeDependentResources();
			CreateWindowSizeDependentResources();
		}

		m_inputDevices.Mouse->SetWindow(windowModeHelper->hWnd);

		windowModeHelper->SetFullscreenResolutionHandledByWindow(false);

		ResetCamera();
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

	void OnWindowSizeChanged() { if (m_deviceResources->WindowSizeChanged(m_windowModeHelper->GetResolution())) CreateWindowSizeDependentResources(); }

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
		if (ImGui::GetIO().BackendRendererUserData != nullptr) ImGui_ImplDX12_Shutdown();

		for (auto& textures : m_textures | views::values) for (auto& texture : get<0>(textures) | views::values) texture.Resource.Reset();

		m_triangleMeshes = {};

		m_renderTextures = {};

		m_shaderBuffers = {};

		m_topLevelAccelerationStructure.reset();
		m_bottomLevelAccelerationStructures = {};

		m_bloom = {};

		m_toneMapping = {};

		m_textureAlphaBlend.reset();

		m_temporalAntiAliasing.reset();

		m_denoisedComposition.reset();
		m_NRD.reset();

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

	unique_ptr<DeviceResources> m_deviceResources = make_unique<DeviceResources>(DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_D32_FLOAT, 2, D3D_FEATURE_LEVEL_12_1, D3D12_RAYTRACING_TIER_1_1, DeviceResources::c_AllowTearing);

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

	struct RenderTextureNames {
		MAKE_NAME(Motion);
		MAKE_NAME(NormalRoughness);
		MAKE_NAME(ViewZ);
		MAKE_NAME(BaseColorMetalness);
		MAKE_NAME(NoisyDiffuse);
		MAKE_NAME(NoisySpecular);
		MAKE_NAME(DenoisedDiffuse);
		MAKE_NAME(DenoisedSpecular);
		MAKE_NAME(Validation);
		MAKE_NAME(HistoryOutput);
		MAKE_NAME(Output);
		MAKE_NAME(FinalOutput);
		MAKE_NAME(Blur);
		MAKE_NAME(Blur1);
		MAKE_NAME(Blur2);
	};

	struct ObjectNames {
		MAKE_NAME(EnvironmentLight);
		MAKE_NAME(Environment);
		MAKE_NAME(Sphere);
		MAKE_NAME(AlienMetal);
		MAKE_NAME(Moon);
		MAKE_NAME(Earth);
		MAKE_NAME(Star);
		MAKE_NAME(HarmonicOscillator);
	};

	struct ResourceDescriptorHeapIndex {
		enum {
			InstanceResourceDescriptorHeapIndices,
			Camera,
			GlobalData, InstanceData,
			MotionSRV, MotionUAV,
			NormalRoughnessSRV, NormalRoughnessUAV,
			ViewZSRV, ViewZUAV,
			BaseColorMetalnessSRV, BaseColorMetalnessUAV,
			NoisyDiffuse, NoisySpecular,
			DenoisedDiffuseSRV, DenoisedDiffuseUAV,
			DenoisedSpecularSRV, DenoisedSpecularUAV,
			ValidationSRV, ValidationUAV,
			HistoryOutputSRV, HistoryOutputUAV,
			OutputSRV, OutputUAV,
			FinalOutputSRV, FinalOutputUAV,
			Blur, Blur1, Blur2,
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

	struct RenderDescriptorHeapIndex {
		enum {
			Blur, Blur1, Blur2,
			Count
		};
	};
	unique_ptr<DescriptorPile> m_renderDescriptorHeap;

	ComPtr<ID3D12RootSignature> m_rootSignature;

	ComPtr<ID3D12PipelineState> m_pipelineState;

	static constexpr UINT MaxTraceRecursionDepth = 32, MaxSamplesPerPixel = 16;

	CommonSettings m_NRDCommonSettings{ .motionVectorScale{ 1, 1, 1 }, .isBaseColorMetalnessAvailable = true, .enableValidation = true };
	ReblurSettings m_NRDReblurSettings{ .hitDistanceReconstructionMode = HitDistanceReconstructionMode::AREA_5X5, .enableAntiFirefly = true };
	unique_ptr<NRD> m_NRD;
	unique_ptr<DenoisedComposition> m_denoisedComposition;

	unique_ptr<TemporalAntiAliasing> m_temporalAntiAliasing;

	unique_ptr<TextureAlphaBlend> m_textureAlphaBlend;

	map<ToneMapPostProcess::Operator, shared_ptr<ToneMapPostProcess>> m_toneMapping;

	static constexpr float BloomMaxBlurSize = 4;
	struct {
		unique_ptr<BasicPostProcess> Extraction, Blur;
		unique_ptr<DualPostProcess> Combination;
	} m_bloom;

	map<string, shared_ptr<TriangleMesh<VertexPositionNormalTexture, UINT16>>, less<>> m_triangleMeshes;

	map<string, shared_ptr<BottomLevelAccelerationStructure>, less<>> m_bottomLevelAccelerationStructures;
	unique_ptr<TopLevelAccelerationStructure> m_topLevelAccelerationStructure;

	struct {
		unique_ptr<ConstantBuffer<GlobalResourceDescriptorHeapIndices>> GlobalResourceDescriptorHeapIndices;
		unique_ptr<ConstantBuffer<Camera>> Camera;
		unique_ptr<ConstantBuffer<GlobalData>> GlobalData;
		unique_ptr<StructuredBuffer<InstanceResourceDescriptorHeapIndices>> InstanceResourceDescriptorHeapIndices;
		unique_ptr<StructuredBuffer<InstanceData>> InstanceData;
	} m_shaderBuffers;

	map<string, shared_ptr<RenderTexture>, less<>> m_renderTextures;

	static constexpr float
		CameraMinVerticalFieldOfView = 30, CameraMaxVerticalFieldOfView = 120,
		CameraMinTranslationSpeed = 1e-1f, CameraMaxTranslationSpeed = 100,
		CameraMinRotationSpeed = 1e-1f, CameraMaxRotationSpeed = 2;
	CameraController m_cameraController;

	HaltonSamplePattern m_haltonSamplePattern;

	struct { bool IsVisible, HasFocus = true, IsSettingsVisible; } m_UIStates{};

	bool m_isWindowSettingChanged{};

	TextureDictionary m_textures;

	struct RenderObject {
		string Name;
		struct {
			struct { UINT Vertices = ~0u, Indices = ~0u; } TriangleMesh;
		} ResourceDescriptorHeapIndices;
		Material Material{};
		TextureDictionary::mapped_type* pTextures{};
		PxShape* Shape{};
		Matrix World, WorldToPreviousWorld;
	};
	vector<RenderObject> m_renderObjects;

	bool m_isSimulatingPhysics = true;
	struct {
		map<string, tuple<PxRigidBody*, bool /*IsGravityEnabled*/>, less<>> RigidBodies;
		const struct { PxReal PositionY = 0.5f, Period = 3; } Spring;
	} m_physicsObjects;
	PhysX m_physX;

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

		{
			const auto CreateTexture = [&](DXGI_FORMAT format, const SIZE& size, LPCSTR textureName, UINT srvDescriptorHeapIndex = ~0u, UINT uavDescriptorHeapIndex = ~0u, UINT rtvDescriptorHeapIndex = ~0u) {
				auto& texture = m_renderTextures[textureName];
				texture = make_shared<RenderTexture>(format);
				texture->SetDevice(device, m_resourceDescriptorHeap.get(), srvDescriptorHeapIndex, uavDescriptorHeapIndex, m_renderDescriptorHeap.get(), rtvDescriptorHeapIndex);
				texture->CreateResource(size.cx, size.cy);
			};

			if (m_NRD = make_unique<NRD>(device, m_deviceResources->GetCommandQueue(), m_deviceResources->GetCommandAllocator(), m_deviceResources->GetCommandList(), m_deviceResources->GetBackBufferCount(), initializer_list{ Method::REBLUR_DIFFUSE_SPECULAR }, m_deviceResources->GetOutputSize());
				m_NRD->IsAvailable()) {
				const auto CreateTexture1 = [&](DXGI_FORMAT format, LPCSTR textureName, ResourceType resourceType, UINT srvDescriptorHeapIndex = ~0u, UINT uavDescriptorHeapIndex = ~0u) {
					CreateTexture(format, outputSize, textureName, srvDescriptorHeapIndex, uavDescriptorHeapIndex);

					const auto& texture = m_renderTextures.at(textureName);
					m_NRD->SetResource(resourceType, texture->GetResource(), texture->GetCurrentState());
				};

				CreateTexture1(DXGI_FORMAT_R16G16B16A16_FLOAT, RenderTextureNames::Motion, ResourceType::IN_MV, ResourceDescriptorHeapIndex::MotionSRV, ResourceDescriptorHeapIndex::MotionUAV);

				CreateTexture1(ToDXGIFormat(GetLibraryDesc().normalEncoding), RenderTextureNames::NormalRoughness, ResourceType::IN_NORMAL_ROUGHNESS, ResourceDescriptorHeapIndex::NormalRoughnessSRV, ResourceDescriptorHeapIndex::NormalRoughnessUAV);

				CreateTexture1(DXGI_FORMAT_R32_FLOAT, RenderTextureNames::ViewZ, ResourceType::IN_VIEWZ, ResourceDescriptorHeapIndex::ViewZSRV, ResourceDescriptorHeapIndex::ViewZUAV);

				CreateTexture1(DXGI_FORMAT_R8G8B8A8_UNORM, RenderTextureNames::BaseColorMetalness, ResourceType::IN_BASECOLOR_METALNESS, ResourceDescriptorHeapIndex::BaseColorMetalnessSRV, ResourceDescriptorHeapIndex::BaseColorMetalnessUAV);

				CreateTexture1(DXGI_FORMAT_R16G16B16A16_FLOAT, RenderTextureNames::NoisyDiffuse, ResourceType::IN_DIFF_RADIANCE_HITDIST, ~0u, ResourceDescriptorHeapIndex::NoisyDiffuse);
				CreateTexture1(DXGI_FORMAT_R16G16B16A16_FLOAT, RenderTextureNames::NoisySpecular, ResourceType::IN_SPEC_RADIANCE_HITDIST, ~0u, ResourceDescriptorHeapIndex::NoisySpecular);

				CreateTexture1(DXGI_FORMAT_R16G16B16A16_FLOAT, RenderTextureNames::DenoisedDiffuse, ResourceType::OUT_DIFF_RADIANCE_HITDIST, ResourceDescriptorHeapIndex::DenoisedDiffuseSRV, ResourceDescriptorHeapIndex::DenoisedDiffuseUAV);
				CreateTexture1(DXGI_FORMAT_R16G16B16A16_FLOAT, RenderTextureNames::DenoisedSpecular, ResourceType::OUT_SPEC_RADIANCE_HITDIST, ResourceDescriptorHeapIndex::DenoisedSpecularSRV, ResourceDescriptorHeapIndex::DenoisedSpecularUAV);

				CreateTexture1(DXGI_FORMAT_R16G16B16A16_FLOAT, RenderTextureNames::Validation, ResourceType::OUT_VALIDATION, ResourceDescriptorHeapIndex::ValidationSRV, ResourceDescriptorHeapIndex::ValidationUAV);

				m_NRD->SetMethodSettings(Method::REBLUR_DIFFUSE_SPECULAR, &m_NRDReblurSettings);
			}

			{
				CreateTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, outputSize, RenderTextureNames::HistoryOutput, ResourceDescriptorHeapIndex::HistoryOutputSRV, ResourceDescriptorHeapIndex::HistoryOutputUAV);
				CreateTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, outputSize, RenderTextureNames::Output, ResourceDescriptorHeapIndex::OutputSRV, ResourceDescriptorHeapIndex::OutputUAV);
				CreateTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, outputSize, RenderTextureNames::FinalOutput, ResourceDescriptorHeapIndex::FinalOutputSRV, ResourceDescriptorHeapIndex::FinalOutputUAV);
			}

			{
				const auto backBufferFormat = m_deviceResources->GetBackBufferFormat();

				CreateTexture(backBufferFormat, outputSize, RenderTextureNames::Blur, ResourceDescriptorHeapIndex::Blur, ~0u, RenderDescriptorHeapIndex::Blur);

				const SIZE size{ outputSize.cx / 2, outputSize.cy / 2 };
				CreateTexture(backBufferFormat, size, RenderTextureNames::Blur1, ResourceDescriptorHeapIndex::Blur1, ~0u, RenderDescriptorHeapIndex::Blur1);
				CreateTexture(backBufferFormat, size, RenderTextureNames::Blur2, ResourceDescriptorHeapIndex::Blur2, ~0u, RenderDescriptorHeapIndex::Blur2);
			}

			m_denoisedComposition->TextureSize = m_temporalAntiAliasing->TextureSize = m_textureAlphaBlend->TextureSize = outputSize;
		}

		m_cameraController.SetLens(XMConvertToRadians(GraphicsSettings.Camera.VerticalFieldOfView), static_cast<float>(outputSize.cx) / static_cast<float>(outputSize.cy));

		{
			auto& IO = ImGui::GetIO();

			if (IO.BackendRendererUserData != nullptr) ImGui_ImplDX12_Shutdown();
			ImGui_ImplDX12_Init(device, static_cast<int>(m_deviceResources->GetBackBufferCount()), m_deviceResources->GetBackBufferFormat(), m_resourceDescriptorHeap->Heap(), m_resourceDescriptorHeap->GetCpuHandle(ResourceDescriptorHeapIndex::Font), m_resourceDescriptorHeap->GetGpuHandle(ResourceDescriptorHeapIndex::Font));

			IO.Fonts->Clear();
			IO.Fonts->AddFontFromFileTTF(R"(C:\Windows\Fonts\segoeui.ttf)", static_cast<float>(outputSize.cy) * 0.022f);
		}
	}

	void Update() {
		PIXBeginEvent(PIX_COLOR_DEFAULT, L"Update");

		{
			auto& camera = m_shaderBuffers.Camera->GetData();

			reinterpret_cast<XMFLOAT2&>(m_NRDCommonSettings.cameraJitter) = camera.PixelJitter = GraphicsSettings.Camera.IsJitterEnabled ? m_haltonSamplePattern.GetNext() : XMFLOAT2();

			camera.PreviousWorldToView = m_cameraController.GetWorldToView();
			camera.PreviousWorldToProjection = m_cameraController.GetWorldToProjection();

			reinterpret_cast<Matrix&>(m_NRDCommonSettings.worldToViewMatrixPrev) = m_cameraController.GetWorldToView();
			reinterpret_cast<Matrix&>(m_NRDCommonSettings.viewToClipMatrixPrev) = m_cameraController.GetViewToProjection();

			ProcessInput();

			camera.Position = m_cameraController.GetPosition();
			camera.RightDirection = m_cameraController.GetRightDirection();
			camera.UpDirection = m_cameraController.GetUpDirection();
			camera.ForwardDirection = m_cameraController.GetForwardDirection();

			reinterpret_cast<Matrix&>(m_NRDCommonSettings.worldToViewMatrix) = m_cameraController.GetWorldToView();
			reinterpret_cast<Matrix&>(m_NRDCommonSettings.viewToClipMatrix) = m_cameraController.GetViewToProjection();
		}

		UpdateGlobalData();

		UpdateRenderObjects();

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

		const auto descriptorHeap = m_resourceDescriptorHeap->Heap();
		commandList->SetDescriptorHeaps(1, &descriptorHeap);

		DispatchRays();

		PostProcess();

		if (m_UIStates.IsVisible) RenderUI();

		PIXEndEvent(commandList);

		PIXBeginEvent(PIX_COLOR_DEFAULT, L"Present");

		m_deviceResources->Present();

		m_deviceResources->WaitForGpu();

		m_graphicsMemory->Commit(m_deviceResources->GetCommandQueue());

		PIXEndEvent();
	}

	void CheckSettings() {
		{
			auto& cameraSettings = GraphicsSettings.Camera;
			cameraSettings.VerticalFieldOfView = clamp(cameraSettings.VerticalFieldOfView, CameraMinVerticalFieldOfView, CameraMaxVerticalFieldOfView);
		}

		{
			auto& raytracingSettings = GraphicsSettings.Raytracing;
			raytracingSettings.MaxTraceRecursionDepth = clamp(raytracingSettings.MaxTraceRecursionDepth, 1u, MaxTraceRecursionDepth);
			raytracingSettings.SamplesPerPixel = clamp(raytracingSettings.SamplesPerPixel, 1u, MaxSamplesPerPixel);
		}

		{
			auto& postProcessingSettings = GraphicsSettings.PostProcessing;

			postProcessingSettings.RaytracingDenoising.SplitScreen = clamp(postProcessingSettings.RaytracingDenoising.SplitScreen, 0.0f, 1.0f);

			{
				auto& bloomSettings = postProcessingSettings.Bloom;
				bloomSettings.Threshold = clamp(bloomSettings.Threshold, 0.0f, 1.0f);
				bloomSettings.BlurSize = clamp(bloomSettings.BlurSize, 1.0f, BloomMaxBlurSize);
			}
		}

		UISettings.WindowOpacity = clamp(UISettings.WindowOpacity, 0.0f, 1.0f);

		{
			auto& speedSettings = ControlsSettings.Camera.Speed;
			speedSettings.Translation = clamp(speedSettings.Translation, CameraMinTranslationSpeed, CameraMaxTranslationSpeed);
			speedSettings.Rotation = clamp(speedSettings.Rotation, CameraMinRotationSpeed, CameraMaxRotationSpeed);
		}
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
			Matrix::CreateFromYawPitchRoll(XM_PI * 0.2f, XM_PI,0)
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
			{}
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
			Matrix::CreateTranslation(0.5f, 0, 0)
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
			{}
		};

		m_textures = move(textures);
	}

	void LoadTextures() {
		const auto device = m_deviceResources->GetD3DDevice();
		const auto commandQueue = m_deviceResources->GetCommandQueue();

		m_textures.Load(device, commandQueue, *m_resourceDescriptorHeap, 8);
	}

	void BuildRenderObjects() {
		decltype(m_renderObjects) renderObjects;

		const auto& material = *m_physX.GetPhysics().createMaterial(0.5f, 0.5f, 0.6f);

		const auto AddRenderObject = [&](RenderObject& renderObject, const auto& transform, const PxSphereGeometry& geometry) -> decltype(auto) {
			renderObject.ResourceDescriptorHeapIndices = {
				.TriangleMesh{
					.Vertices = ResourceDescriptorHeapIndex::SphereVertices,
					.Indices = ResourceDescriptorHeapIndex::SphereIndices
				}
			};

			auto& rigidDynamic = *m_physX.GetPhysics().createRigidDynamic(PxTransform(transform));

			renderObject.Shape = PxRigidActorExt::createExclusiveShape(rigidDynamic, geometry, material);

			PxRigidBodyExt::updateMassAndInertia(rigidDynamic, 1);

			rigidDynamic.setAngularDamping(0);

			m_physX.GetScene().addActor(rigidDynamic);

			renderObjects.emplace_back(renderObject);

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
				RenderObject renderObject;

				if (Name != nullptr && m_textures.contains(Name)) renderObject.pTextures = &m_textures.at(Name);

				renderObject.Material = Material;

				AddRenderObject(renderObject, Position, PxSphereGeometry(0.5f));
			}

			Random random;
			for (const auto i : views::iota(-10, 11)) {
				for (const auto j : views::iota(-10, 11)) {
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

					RenderObject renderObject;

					renderObject.Name = ObjectNames::HarmonicOscillator;

					constexpr auto RandomFloat4 = [&](float min) {
						const auto value = random.Float3();
						return XMFLOAT4(value.x, value.y, value.z, 1);
					};
					if (const auto randomValue = random.Float();
						randomValue < 0.3f) {
						renderObject.Material = { .BaseColor = RandomFloat4(0.1f) };
					}
					else if (randomValue < 0.6f) {
						renderObject.Material = {
							.BaseColor = RandomFloat4(0.1f),
							.Metallic = 1,
							.Roughness = random.Float(0, 0.5f)
						};
					}
					else if (randomValue < 0.8f) {
						renderObject.Material = {
							.BaseColor = RandomFloat4(0.1f),
							.Roughness = random.Float(0, 0.5f),
							.Opacity = 0,
							.RefractiveIndex = 1.5f
						};
					}
					else {
						renderObject.Material = {
							.BaseColor = RandomFloat4(0.1f),
							.EmissiveColor = RandomFloat4(0.2f),
							.Metallic = random.Float(0.4f),
							.Roughness = random.Float(0.3f)
						};
					}

					auto& rigidDynamic = AddRenderObject(renderObject, position, PxSphereGeometry(0.075f));
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
				RenderObject renderObject;

				renderObject.Name = Name;

				renderObject.Material = Material;

				if (m_textures.contains(Name)) renderObject.pTextures = &m_textures.at(Name);

				auto& rigidDynamic = AddRenderObject(renderObject, Position, PxSphereGeometry(Radius));
				if (renderObject.Name == ObjectNames::Moon) {
					const auto x = earth.Position - Position;
					const auto magnitude = x.magnitude();
					const auto normalized = x / magnitude;
					const auto linearSpeed = UniversalGravitation::CalculateFirstCosmicSpeed(earth.Mass, magnitude);
					rigidDynamic.setLinearVelocity(linearSpeed * PxVec3(-normalized.z, 0, normalized.x));
					rigidDynamic.setAngularVelocity({ 0, linearSpeed / magnitude, 0 });
				}
				else if (renderObject.Name == ObjectNames::Earth) {
					rigidDynamic.setAngularVelocity({ 0, PxTwoPi / RotationPeriod, 0 });
					PxRigidBodyExt::setMassAndUpdateInertia(rigidDynamic, &Mass, 1);
				}
				else if (renderObject.Name == ObjectNames::Star) rigidDynamic.setMass(0);

				m_physicsObjects.RigidBodies[Name] = { &rigidDynamic, false };
			}
		}

		m_renderObjects = move(renderObjects);
	}

	void CreateDescriptorHeaps() {
		const auto device = m_deviceResources->GetD3DDevice();

		m_resourceDescriptorHeap = make_unique<DescriptorPile>(device, ResourceDescriptorHeapIndex::Count);

		m_renderDescriptorHeap = make_unique<DescriptorPile>(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, RenderDescriptorHeapIndex::Count);
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

		{
			m_denoisedComposition = make_unique<DenoisedComposition>(device);
			m_denoisedComposition->TextureDescriptors = {
				.NormalRoughnessSRV = m_resourceDescriptorHeap->GetGpuHandle(ResourceDescriptorHeapIndex::NormalRoughnessSRV),
				.ViewZSRV = m_resourceDescriptorHeap->GetGpuHandle(ResourceDescriptorHeapIndex::ViewZSRV),
				.BaseColorMetalnessSRV = m_resourceDescriptorHeap->GetGpuHandle(ResourceDescriptorHeapIndex::BaseColorMetalnessSRV),
				.DenoisedDiffuseSRV = m_resourceDescriptorHeap->GetGpuHandle(ResourceDescriptorHeapIndex::DenoisedDiffuseSRV),
				.DenoisedSpecularSRV = m_resourceDescriptorHeap->GetGpuHandle(ResourceDescriptorHeapIndex::DenoisedSpecularSRV),
				.OutputUAV = m_resourceDescriptorHeap->GetGpuHandle(ResourceDescriptorHeapIndex::OutputUAV)
			};
		}

		{
			m_temporalAntiAliasing = make_unique<TemporalAntiAliasing>(device);
			m_temporalAntiAliasing->TextureDescriptors = {
				.MotionSRV = m_resourceDescriptorHeap->GetGpuHandle(ResourceDescriptorHeapIndex::MotionSRV),
				.HistoryOutputSRV = m_resourceDescriptorHeap->GetGpuHandle(ResourceDescriptorHeapIndex::HistoryOutputSRV),
				.OutputSRV = m_resourceDescriptorHeap->GetGpuHandle(ResourceDescriptorHeapIndex::OutputSRV),
				.FinalOutputUAV = m_resourceDescriptorHeap->GetGpuHandle(ResourceDescriptorHeapIndex::FinalOutputUAV)
			};
		}

		{
			m_textureAlphaBlend = make_unique<TextureAlphaBlend>(device);
			m_textureAlphaBlend->TextureDescriptors.ForegroundSRV = m_resourceDescriptorHeap->GetGpuHandle(ResourceDescriptorHeapIndex::ValidationSRV);
		}

		const auto backBufferFormat = m_deviceResources->GetBackBufferFormat();

		m_toneMapping[ToneMapPostProcess::None] = make_shared<ToneMapPostProcess>(device, RenderTargetState(backBufferFormat, DXGI_FORMAT_UNKNOWN), ToneMapPostProcess::None, ToneMapPostProcess::Linear);

		{
			const RenderTargetState renderTargetState(backBufferFormat, m_deviceResources->GetDepthBufferFormat());
			m_bloom = {
				.Extraction = make_unique<BasicPostProcess>(device, renderTargetState, BasicPostProcess::BloomExtract),
				.Blur = make_unique<BasicPostProcess>(device, renderTargetState, BasicPostProcess::BloomBlur),
				.Combination = make_unique<DualPostProcess>(device, renderTargetState, DualPostProcess::BloomCombine),
			};
		}
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
		instanceDescs.reserve(size(m_renderObjects));
		for (UINT i = 0; const auto & renderObject : m_renderObjects) {
			LPCSTR objectName;
			switch (renderObject.Shape->getGeometryType()) {
				case PxGeometryType::eSPHERE: objectName = ObjectNames::Sphere; break;
				default: throw;
			}

			D3D12_RAYTRACING_INSTANCE_DESC instanceDesc;
			XMStoreFloat3x4(reinterpret_cast<XMFLOAT3X4*>(instanceDesc.Transform), renderObject.World);
			instanceDesc.InstanceID = i++;
			instanceDesc.InstanceMask = ~0u;
			instanceDesc.InstanceContributionToHitGroupIndex = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
			instanceDesc.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_FRONT_COUNTERCLOCKWISE;
			instanceDesc.AccelerationStructure = m_bottomLevelAccelerationStructures.at(objectName)->GetBuffer()->GetGPUVirtualAddress();
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

			Matrix environmentLightCubeMapTransform, environmentCubeMapTransform;

			{
				const auto GetTexture = [&](LPCSTR name, UINT& srvDescriptorHeapIndex, Matrix& transform) {
					if (const auto pTextures = m_textures.find(name); pTextures != cend(m_textures)) {
						const auto& textures = get<0>(pTextures->second);
						if (const auto pTexture = textures.find(TextureType::CubeMap); pTexture != cend(textures)) {
							srvDescriptorHeapIndex = pTexture->second.DescriptorHeapIndices.SRV;
							transform = get<1>(pTextures->second);
							return;
						}
					}
					srvDescriptorHeapIndex = ~0u;
				};

				UINT environmentLightCubeMapDescriptorHeapIndex, environmentCubeMapDescriptorHeapIndex;
				GetTexture(ObjectNames::EnvironmentLight, environmentLightCubeMapDescriptorHeapIndex, environmentLightCubeMapTransform);
				GetTexture(ObjectNames::Environment, environmentCubeMapDescriptorHeapIndex, environmentCubeMapTransform);

				CreateBuffer(
					m_shaderBuffers.GlobalResourceDescriptorHeapIndices,
					GlobalResourceDescriptorHeapIndices{
						.InstanceResourceDescriptorHeapIndices = ResourceDescriptorHeapIndex::InstanceResourceDescriptorHeapIndices,
						.Camera = ResourceDescriptorHeapIndex::Camera,
						.GlobalData = ResourceDescriptorHeapIndex::GlobalData,
						.InstanceData = ResourceDescriptorHeapIndex::InstanceData,
						.Motion = ResourceDescriptorHeapIndex::MotionUAV,
						.NormalRoughness = ResourceDescriptorHeapIndex::NormalRoughnessUAV,
						.ViewZ = ResourceDescriptorHeapIndex::ViewZUAV,
						.BaseColorMetalness = ResourceDescriptorHeapIndex::BaseColorMetalnessUAV,
						.NoisyDiffuse = ResourceDescriptorHeapIndex::NoisyDiffuse,
						.NoisySpecular = ResourceDescriptorHeapIndex::NoisySpecular,
						.Output = ResourceDescriptorHeapIndex::OutputUAV,
						.EnvironmentLightCubeMap = environmentLightCubeMapDescriptorHeapIndex,
						.EnvironmentCubeMap = environmentCubeMapDescriptorHeapIndex
					}
				);
			}

			CreateBuffer(
				m_shaderBuffers.Camera,
				Camera{
					.Position = m_cameraController.GetPosition(),
					.NearZ = m_cameraController.GetNearZ(),
					.FarZ = m_cameraController.GetFarZ()
				},
				ResourceDescriptorHeapIndex::Camera
			);

			CreateBuffer(
				m_shaderBuffers.GlobalData,
				GlobalData{
					.NRDHitDistanceParameters = reinterpret_cast<const XMFLOAT4&>(m_NRDReblurSettings.hitDistanceParameters),
					.EnvironmentLightColor{ 0, 0, 0, -1 },
					.EnvironmentLightCubeMapTransform = environmentLightCubeMapTransform,
					.EnvironmentColor{ 0, 0, 0, -1 },
					.EnvironmentCubeMapTransform = environmentCubeMapTransform
				},
				ResourceDescriptorHeapIndex::GlobalData
			);
		}

		if (const auto instanceDescCount = m_topLevelAccelerationStructure->GetDescCount()) {
			const auto CreateBuffer = [&]<typename T>(unique_ptr<T>&uploadBuffer, UINT count, UINT descriptorHeapIndex) {
				uploadBuffer = make_unique<T>(device, count);
				uploadBuffer->CreateShaderResourceView(m_resourceDescriptorHeap->GetCpuHandle(descriptorHeapIndex));
			};

			CreateBuffer(m_shaderBuffers.InstanceResourceDescriptorHeapIndices, instanceDescCount, ResourceDescriptorHeapIndex::InstanceResourceDescriptorHeapIndices);

			CreateBuffer(m_shaderBuffers.InstanceData, instanceDescCount, ResourceDescriptorHeapIndex::InstanceData);

			for (const auto i : views::iota(0u, instanceDescCount)) {
				const auto& renderObject = m_renderObjects[i];

				auto& instanceResourceDescriptorHeapIndices = (*m_shaderBuffers.InstanceResourceDescriptorHeapIndices)[i];

				instanceResourceDescriptorHeapIndices = {
					.TriangleMesh{
						.Vertices = renderObject.ResourceDescriptorHeapIndices.TriangleMesh.Vertices,
						.Indices = renderObject.ResourceDescriptorHeapIndices.TriangleMesh.Indices
					}
				};

				auto& instanceData = (*m_shaderBuffers.InstanceData)[i];

				instanceData.MaterialBaseColorAlphaThreshold = 0.94f;
				instanceData.Material = renderObject.Material;

				if (renderObject.pTextures != nullptr) {
					for (const auto& [textureType, texture] : get<0>(*renderObject.pTextures)) {
						auto& textures = instanceResourceDescriptorHeapIndices.Textures;
						UINT* p;
						switch (textureType) {
							case TextureType::BaseColorMap: p = &textures.BaseColorMap; break;
							case TextureType::EmissiveColorMap: p = &textures.EmissiveColorMap; break;
							case TextureType::MetallicMap: p = &textures.MetallicMap; break;
							case TextureType::RoughnessMap: p = &textures.RoughnessMap; break;
							case TextureType::AmbientOcclusionMap: p = &textures.AmbientOcclusionMap; break;
							case TextureType::OpacityMap: p = &textures.OpacityMap; break;
							case TextureType::NormalMap: p = &textures.NormalMap; break;
							default: p = nullptr; break;
						}
						if (p != nullptr) *p = texture.DescriptorHeapIndices.SRV;
					}

					instanceData.TextureTransform = get<1>(*renderObject.pTextures);
				}

				instanceData.WorldToPreviousWorld = Matrix();
			}
		}
	}

	void ProcessInput() {
		auto& gamepadStateTracker = m_inputDeviceStateTrackers.Gamepad;
		if (const auto gamepadState = m_inputDevices.Gamepad->GetState(0); gamepadState.IsConnected()) gamepadStateTracker.Update(gamepadState);
		else gamepadStateTracker.Reset();

		auto& keyboardStateTracker = m_inputDeviceStateTrackers.Keyboard;
		keyboardStateTracker.Update(m_inputDevices.Keyboard->GetState());

		auto& mouseStateTracker = m_inputDeviceStateTrackers.Mouse;
		mouseStateTracker.Update(m_inputDevices.Mouse->GetState());

		{
			if (gamepadStateTracker.menu == GamepadButtonState::PRESSED) m_UIStates.IsVisible = !m_UIStates.IsVisible;
			if (keyboardStateTracker.IsKeyPressed(Key::Escape)) m_UIStates.IsVisible = !m_UIStates.IsVisible;
		}

		if (auto& IO = ImGui::GetIO(); m_UIStates.IsVisible) {
			if (IO.WantCaptureKeyboard) m_UIStates.HasFocus = true;
			if (IO.WantCaptureMouse) m_UIStates.HasFocus = true;
			else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) m_UIStates.HasFocus = false;

			if (m_UIStates.HasFocus) {
				IO.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;

				m_inputDevices.Mouse->SetMode(Mouse::MODE_ABSOLUTE);
			}
			else IO.ConfigFlags |= ImGuiConfigFlags_NoMouse;
		}

		if (!m_UIStates.IsVisible || !m_UIStates.HasFocus) {
			{
				if (gamepadStateTracker.a == GamepadButtonState::PRESSED) m_isSimulatingPhysics = !m_isSimulatingPhysics;
				if (keyboardStateTracker.IsKeyPressed(Key::Space)) m_isSimulatingPhysics = !m_isSimulatingPhysics;
			}

			{
				auto& isGravityEnabled = get<1>(m_physicsObjects.RigidBodies.at(ObjectNames::Earth));
				if (gamepadStateTracker.b == GamepadButtonState::PRESSED) isGravityEnabled = !isGravityEnabled;
				if (keyboardStateTracker.IsKeyPressed(Key::G)) isGravityEnabled = !isGravityEnabled;
			}

			{
				auto& isGravityEnabled = get<1>(m_physicsObjects.RigidBodies.at(ObjectNames::Star));
				if (gamepadStateTracker.y == GamepadButtonState::PRESSED) isGravityEnabled = !isGravityEnabled;
				if (keyboardStateTracker.IsKeyPressed(Key::H)) isGravityEnabled = !isGravityEnabled;
			}

			m_inputDevices.Mouse->SetMode(Mouse::MODE_RELATIVE);

			UpdateCamera();
		}
	}

	void ResetCamera() {
		m_cameraController.SetPosition({ 0, 0, -15 });
		m_cameraController.SetRotation({});
	}

	void UpdateCamera() {
		const auto elapsedSeconds = static_cast<float>(m_stepTimer.GetElapsedSeconds());

		const auto gamepadState = m_inputDeviceStateTrackers.Gamepad.GetLastState();
		const auto keyboardState = m_inputDeviceStateTrackers.Keyboard.GetLastState();
		const auto mouseState = m_inputDeviceStateTrackers.Mouse.GetLastState();

		Vector3 translation;
		float yaw = 0, pitch = 0;

		const auto& speedSettings = ControlsSettings.Camera.Speed;

		if (gamepadState.IsConnected()) {
			if (m_inputDeviceStateTrackers.Gamepad.view == GamepadButtonState::PRESSED) ResetCamera();

			const auto translationSpeed = elapsedSeconds * speedSettings.Translation * (gamepadState.IsLeftTriggerPressed() ? 0.5f : 1.0f) * (gamepadState.IsRightTriggerPressed() ? 2.0f : 1.0f);
			translation.x += gamepadState.thumbSticks.leftX * translationSpeed;
			translation.z += gamepadState.thumbSticks.leftY * translationSpeed;

			const auto rotationSpeed = elapsedSeconds * XM_2PI * speedSettings.Rotation;
			yaw += gamepadState.thumbSticks.rightX * rotationSpeed;
			pitch += gamepadState.thumbSticks.rightY * rotationSpeed;
		}

		if (mouseState.positionMode == Mouse::MODE_RELATIVE) {
			if (m_inputDeviceStateTrackers.Keyboard.IsKeyPressed(Key::Home)) ResetCamera();

			const auto translationSpeed = elapsedSeconds * speedSettings.Translation * (keyboardState.LeftControl ? 0.5f : 1.0f) * (keyboardState.LeftShift ? 2.0f : 1.0f);
			if (keyboardState.A) translation.x -= translationSpeed;
			if (keyboardState.D) translation.x += translationSpeed;
			if (keyboardState.W) translation.z += translationSpeed;
			if (keyboardState.S) translation.z -= translationSpeed;

			const auto rotationSpeed = elapsedSeconds * XM_2PI * 0.022f * speedSettings.Rotation;
			yaw += static_cast<float>(mouseState.x) * rotationSpeed;
			pitch += static_cast<float>(-mouseState.y) * rotationSpeed;
		}

		if (translation.x != 0 || translation.y != 0 || translation.z != 0) {
			constexpr auto ToPxVec3 = [](const XMFLOAT3& value) { return PxVec3(value.x, value.y, -value.z); };

			auto x = ToPxVec3(m_cameraController.GetNormalizedRightDirection() * translation.x + m_cameraController.GetNormalizedUpDirection() * translation.y + m_cameraController.GetNormalizedForwardDirection() * translation.z);
			const auto magnitude = x.magnitude();
			const auto normalized = x / magnitude;

			if (PxRaycastBuffer raycastBuffer;
				m_physX.GetScene().raycast(ToPxVec3(m_cameraController.GetPosition()), normalized, magnitude + 0.1f, raycastBuffer) && raycastBuffer.block.distance < magnitude) {
				x = normalized * max(0.0f, raycastBuffer.block.distance - 0.1f);
			}

			translation = { x.x, x.y, -x.z };
		}

		if (const auto angle = asin(m_cameraController.GetNormalizedForwardDirection().y) + pitch;
			angle > XM_PIDIV2) pitch = max(0.0f, -angle + XM_PIDIV2 - 0.1f);
		else if (angle < -XM_PIDIV2) pitch = min(0.0f, -angle - XM_PIDIV2 + 0.1f);

		if (translation.x == 0 && translation.y == 0 && translation.z == 0 && yaw == 0 && pitch == 0) return;

		m_cameraController.Translate(translation);
		m_cameraController.Rotate(yaw, pitch);
	}

	void UpdateGlobalData() {
		auto& globalData = m_shaderBuffers.GlobalData->GetData();

		globalData.FrameIndex = m_stepTimer.GetFrameCount() - 1;

		{
			const auto& raytracingSettings = GraphicsSettings.Raytracing;
			globalData.IsRussianRouletteEnabled = raytracingSettings.IsRussianRouletteEnabled;
			globalData.MaxTraceRecursionDepth = raytracingSettings.MaxTraceRecursionDepth;
			globalData.SamplesPerPixel = raytracingSettings.SamplesPerPixel;
		}
	}

	void UpdateRenderObjects() {
		if (!m_isSimulatingPhysics && m_stepTimer.GetFrameCount() > 1) return;

		const auto Transform = [&](const PxShape& shape) {
			PxVec3 scaling;
			switch (const auto geometry = shape.getGeometry(); shape.getGeometryType()) {
				case PxGeometryType::eSPHERE: scaling = PxVec3(geometry.sphere().radius * 2); break;
				default: throw;
			}

			PxMat44 world(PxVec4(1, 1, -1, 1));
			world *= PxShapeExt::getGlobalPose(shape, *shape.getActor());
			world.scale(PxVec4(scaling, 1));
			return reinterpret_cast<const Matrix&>(*world.front());
		};

		for (auto& renderObject : m_renderObjects) {
			renderObject.WorldToPreviousWorld = Transform(*renderObject.Shape);

			const auto& shape = *renderObject.Shape;

			const auto rigidBody = shape.getActor()->is<PxRigidBody>();
			if (rigidBody == nullptr) continue;

			const auto mass = rigidBody->getMass();
			if (!mass) continue;

			const auto& position = PxShapeExt::getGlobalPose(shape, *shape.getActor()).p;

			if (const auto& [PositionY, Period] = m_physicsObjects.Spring;
				renderObject.Name == ObjectNames::HarmonicOscillator) {
				const auto k = SimpleHarmonicMotion::Spring::CalculateConstant(mass, Period);
				const PxVec3 x(0, position.y - PositionY, 0);
				rigidBody->addForce(-k * x);
			}

			if (const auto& earth = m_physicsObjects.RigidBodies.at(ObjectNames::Earth);
				(get<1>(earth) && renderObject.Name != ObjectNames::Earth)
				|| renderObject.Name == ObjectNames::Moon) {
				const auto& earthRigidBody = *get<0>(earth);
				const auto x = earthRigidBody.getGlobalPose().p - position;
				const auto magnitude = x.magnitude();
				const auto normalized = x / magnitude;
				rigidBody->addForce(UniversalGravitation::CalculateAccelerationMagnitude(earthRigidBody.getMass(), magnitude) * normalized, PxForceMode::eACCELERATION);
			}

			if (const auto& star = m_physicsObjects.RigidBodies.at(ObjectNames::Star);
				get<1>(star) && renderObject.Name != ObjectNames::Star) {
				const auto x = get<0>(star)->getGlobalPose().p - position;
				const auto normalized = x.getNormalized();
				rigidBody->addForce(10 * normalized, PxForceMode::eACCELERATION);
			}
		}

		m_physX.Tick(static_cast<PxReal>(min(1.0 / 60, m_stepTimer.GetElapsedSeconds())));

		for (auto& renderObject : m_renderObjects) {
			renderObject.World = Transform(*renderObject.Shape);
			renderObject.WorldToPreviousWorld = renderObject.World.Invert() * renderObject.WorldToPreviousWorld;
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

	void PostProcess() {
		const auto commandList = m_deviceResources->GetCommandList();

		const auto& postProcessingSettings = GraphicsSettings.PostProcessing;

		const auto isNRDEnabled = postProcessingSettings.RaytracingDenoising.IsEnabled && postProcessingSettings.RaytracingDenoising.SplitScreen != 1 && m_NRD->IsAvailable();

		if (isNRDEnabled) {
			Denoise();

			m_NRDCommonSettings.accumulationMode = AccumulationMode::CONTINUE;
		}
		else m_NRDCommonSettings.accumulationMode = AccumulationMode::RESTART;

		if (postProcessingSettings.IsTemporalAntiAliasingEnabled) ProcessTemporalAntiAliasing();

		const auto& output = m_renderTextures[postProcessingSettings.IsTemporalAntiAliasingEnabled ? RenderTextureNames::FinalOutput : RenderTextureNames::Output];

		if (isNRDEnabled && postProcessingSettings.RaytracingDenoising.IsValidationLayerEnabled) {
			const auto& validation = *m_renderTextures.at(RenderTextureNames::Validation);

			const ScopedBarrier scopedBarrier(commandList, { CD3DX12_RESOURCE_BARRIER::Transition(validation.GetResource(), validation.GetCurrentState(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) });

			m_textureAlphaBlend->TextureDescriptors.BackgroundUAV = m_resourceDescriptorHeap->GetGpuHandle(output->GetUavDescriptorHeapIndex());

			m_textureAlphaBlend->Process(commandList);
		}

		const auto isBloomEnabled = postProcessingSettings.Bloom.IsEnabled;

		if (isBloomEnabled) {
			const auto blurRTV = m_renderDescriptorHeap->GetCpuHandle(m_renderTextures.at(RenderTextureNames::Blur)->GetRtvDescriptorHeapIndex());
			commandList->OMSetRenderTargets(1, &blurRTV, FALSE, nullptr);
		}

		{
			auto& toneMapping = *m_toneMapping.at(ToneMapPostProcess::None);
			toneMapping.SetHDRSourceTexture(m_resourceDescriptorHeap->GetGpuHandle(output->GetSrvDescriptorHeapIndex()));
			toneMapping.Process(commandList);
		}

		if (isBloomEnabled) ProcessBloom();
	}

	void Denoise() {
		const auto& raytracingDenoisingSettings = GraphicsSettings.PostProcessing.RaytracingDenoising;

		m_NRDCommonSettings.splitScreen = raytracingDenoisingSettings.SplitScreen;

		m_NRDCommonSettings.enableValidation = raytracingDenoisingSettings.IsValidationLayerEnabled;

		m_NRD->Denoise(m_stepTimer.GetFrameCount() - 1, m_NRDCommonSettings);

		const auto commandList = m_deviceResources->GetCommandList();

		const auto descriptorHeap = m_resourceDescriptorHeap->Heap();
		commandList->SetDescriptorHeaps(1, &descriptorHeap);

		{
			const auto
				& normalRoughness = *m_renderTextures.at(RenderTextureNames::NormalRoughness),
				& viewZ = *m_renderTextures.at(RenderTextureNames::ViewZ),
				& baseColorMetalness = *m_renderTextures.at(RenderTextureNames::BaseColorMetalness),
				& denoisedDiffuse = *m_renderTextures.at(RenderTextureNames::DenoisedDiffuse),
				& denoisedSpecular = *m_renderTextures.at(RenderTextureNames::DenoisedSpecular);

			const ScopedBarrier scopedBarrier(
				commandList,
				{
					CD3DX12_RESOURCE_BARRIER::Transition(normalRoughness.GetResource(), normalRoughness.GetCurrentState(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(viewZ.GetResource(), viewZ.GetCurrentState(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(baseColorMetalness.GetResource(), baseColorMetalness.GetCurrentState(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(denoisedDiffuse.GetResource(), denoisedDiffuse.GetCurrentState(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(denoisedSpecular.GetResource(), denoisedSpecular.GetCurrentState(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
				}
			);

			m_denoisedComposition->GetData() = {
				.CameraPosition = m_cameraController.GetPosition(),
				.CameraRightDirection = m_cameraController.GetRightDirection(),
				.CameraUpDirection = m_cameraController.GetUpDirection(),
				.CameraForwardDirection = m_cameraController.GetForwardDirection(),
				.CameraPixelJitter = m_shaderBuffers.Camera->GetData().PixelJitter
			};

			m_denoisedComposition->Process(commandList);
		}
	}

	void ProcessTemporalAntiAliasing() {
		const auto commandList = m_deviceResources->GetCommandList();

		const auto& historyOutput = *m_renderTextures[RenderTextureNames::HistoryOutput];

		{
			const auto& motion = *m_renderTextures[RenderTextureNames::Motion], & output = *m_renderTextures[RenderTextureNames::Output];

			const ScopedBarrier scopedBarrier(
				commandList,
				{
					CD3DX12_RESOURCE_BARRIER::Transition(motion.GetResource(), motion.GetCurrentState(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(historyOutput.GetResource(), historyOutput.GetCurrentState(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(output.GetResource(), output.GetCurrentState(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
				}
			);

			m_temporalAntiAliasing->GetData() = {
				.FrameIndex = m_stepTimer.GetFrameCount() - 1,
				.CameraPosition = m_cameraController.GetPosition(),
				.CameraRightDirection = m_cameraController.GetRightDirection(),
				.CameraUpDirection = m_cameraController.GetUpDirection(),
				.CameraForwardDirection = m_cameraController.GetForwardDirection(),
				.CameraNearZ = m_cameraController.GetNearZ(),
				.PreviousWorldToProjection = m_shaderBuffers.Camera->GetData().PreviousWorldToProjection
			};

			m_temporalAntiAliasing->Process(commandList);
		}

		{
			const auto& finalOutput = *m_renderTextures[RenderTextureNames::FinalOutput];

			const ScopedBarrier scopedBarrier(
				commandList,
				{
					CD3DX12_RESOURCE_BARRIER::Transition(historyOutput.GetResource(), historyOutput.GetCurrentState(), D3D12_RESOURCE_STATE_COPY_DEST),
					CD3DX12_RESOURCE_BARRIER::Transition(finalOutput.GetResource(), finalOutput.GetCurrentState(), D3D12_RESOURCE_STATE_COPY_SOURCE)
				}
			);

			commandList->CopyResource(historyOutput.GetResource(), finalOutput.GetResource());
		}
	}

	void ProcessBloom() {
		const auto commandList = m_deviceResources->GetCommandList();

		const auto& bloomSettings = GraphicsSettings.PostProcessing.Bloom;

		const auto& [Extraction, Blur, Combination] = m_bloom;

		auto
			& blur = *m_renderTextures.at(RenderTextureNames::Blur),
			& blur1 = *m_renderTextures.at(RenderTextureNames::Blur1),
			& blur2 = *m_renderTextures.at(RenderTextureNames::Blur2);

		const auto blurResource = blur.GetResource(), blur1Resource = blur1.GetResource(), blur2Resource = blur2.GetResource();

		const auto blurState = blur.GetCurrentState(), blur1State = blur1.GetCurrentState(), blur2State = blur2.GetCurrentState();

		const auto
			blurSRV = m_resourceDescriptorHeap->GetGpuHandle(blur.GetSrvDescriptorHeapIndex()),
			blur1SRV = m_resourceDescriptorHeap->GetGpuHandle(blur1.GetSrvDescriptorHeapIndex()),
			blur2SRV = m_resourceDescriptorHeap->GetGpuHandle(blur2.GetSrvDescriptorHeapIndex());

		const auto
			blur1RTV = m_renderDescriptorHeap->GetCpuHandle(blur1.GetRtvDescriptorHeapIndex()),
			blur2RTV = m_renderDescriptorHeap->GetCpuHandle(blur2.GetRtvDescriptorHeapIndex());

		blur.TransitionTo(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

		Extraction->SetSourceTexture(blurSRV, blurResource);
		Extraction->SetBloomExtractParameter(bloomSettings.Threshold);

		commandList->OMSetRenderTargets(1, &blur1RTV, FALSE, nullptr);

		const auto viewPort = m_deviceResources->GetScreenViewport();

		auto halfViewPort = viewPort;
		halfViewPort.Height /= 2;
		halfViewPort.Width /= 2;
		commandList->RSSetViewports(1, &halfViewPort);

		Extraction->Process(commandList);

		blur1.TransitionTo(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

		Blur->SetSourceTexture(blur1SRV, blur1Resource);
		Blur->SetBloomBlurParameters(true, bloomSettings.BlurSize, 1);

		commandList->OMSetRenderTargets(1, &blur2RTV, FALSE, nullptr);

		Blur->Process(commandList);

		blur1.TransitionTo(commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);
		blur2.TransitionTo(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

		Blur->SetSourceTexture(blur2SRV, blur2Resource);
		Blur->SetBloomBlurParameters(false, bloomSettings.BlurSize, 1);

		commandList->OMSetRenderTargets(1, &blur1RTV, FALSE, nullptr);

		Blur->Process(commandList);

		blur1.TransitionTo(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

		Combination->SetSourceTexture(blurSRV);
		Combination->SetSourceTexture2(blur1SRV);
		Combination->SetBloomCombineParameters(1.25f, 1, 1, 1);

		const auto renderTargetView = m_deviceResources->GetRenderTargetView();
		commandList->OMSetRenderTargets(1, &renderTargetView, FALSE, nullptr);

		commandList->RSSetViewports(1, &viewPort);

		Combination->Process(commandList);

		blur.TransitionTo(commandList, blurState);
		blur1.TransitionTo(commandList, blur1State);
		blur2.TransitionTo(commandList, blur2State);
	}

	void RenderUI() {
		const auto outputSize = GetOutputSize();

		ImGui_ImplDX12_NewFrame();
		ImGui_ImplWin32_NewFrame();

		ImGui::GetIO().DisplaySize = { static_cast<float>(outputSize.cx), static_cast<float>(outputSize.cy) };

		ImGui::NewFrame();

		LPCSTR openPopupModalName = nullptr;

		if (ImGui::BeginMainMenuBar()) {
			if (ImGui::GetFrameCount() == 1) ImGui::SetKeyboardFocusHere();

			const auto PopupModal = [&](LPCSTR name) { if (ImGui::MenuItem(name)) openPopupModalName = name; };

			if (ImGui::BeginMenu("File")) {
				if (ImGui::MenuItem("Exit")) PostQuitMessage(ERROR_SUCCESS);

				ImGui::EndMenu();
			}

			if (ImGui::BeginMenu("View")) {
				m_UIStates.IsSettingsVisible |= ImGui::MenuItem("Settings");

				ImGui::EndMenu();
			}

			if (ImGui::BeginMenu("Help")) {
				PopupModal("Controls");

				ImGui::Separator();

				PopupModal("About");

				ImGui::EndMenu();
			}

			ImGui::EndMainMenuBar();
		}

		const auto& viewport = *ImGui::GetMainViewport();

		if (m_UIStates.IsSettingsVisible) {
			ImGui::SetNextWindowBgAlpha(UISettings.WindowOpacity);

			ImGui::SetNextWindowPos({ viewport.WorkPos.x, viewport.WorkPos.y });
			ImGui::SetNextWindowSize({});

			if (ImGui::Begin("Settings", &m_UIStates.IsSettingsVisible, ImGuiWindowFlags_HorizontalScrollbar)) {
				if (ImGui::TreeNode("Graphics")) {
					auto isChanged = false;

					{
						const auto WindowModes = { "Windowed", "Borderless", "Fullscreen" };
						if (auto windowMode = m_windowModeHelper->GetMode();
							ImGui::Combo("Window Mode", reinterpret_cast<int*>(&windowMode), data(WindowModes), static_cast<int>(size(WindowModes)))) {
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

					{
						const ImGuiEx::ScopedEnablement scopedEnablement(m_deviceResources->GetDeviceOptions() & DeviceResources::c_AllowTearing);

						if (auto isEnabled = m_deviceResources->IsVSyncEnabled(); ImGui::Checkbox("V-Sync", &isEnabled) && m_deviceResources->EnableVSync(isEnabled)) {
							GraphicsSettings.IsVSyncEnabled = isEnabled;

							isChanged = true;
						}
					}

					if (ImGui::TreeNodeEx("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
						auto& cameraSettings = GraphicsSettings.Camera;

						isChanged |= ImGui::Checkbox("Jitter", &cameraSettings.IsJitterEnabled);

						if (ImGui::SliderFloat("Vertical Field of View", &cameraSettings.VerticalFieldOfView, CameraMinVerticalFieldOfView, CameraMaxVerticalFieldOfView, "%.1f", ImGuiSliderFlags_AlwaysClamp)) {
							m_cameraController.SetLens(XMConvertToRadians(cameraSettings.VerticalFieldOfView), static_cast<float>(outputSize.cx) / static_cast<float>(outputSize.cy));

							isChanged = true;
						}

						ImGui::TreePop();
					}

					if (ImGui::TreeNodeEx("Raytracing", ImGuiTreeNodeFlags_DefaultOpen)) {
						auto& raytracingSettings = GraphicsSettings.Raytracing;

						isChanged |= ImGui::Checkbox("Russian Roulette", &raytracingSettings.IsRussianRouletteEnabled);

						isChanged |= ImGui::SliderInt("Max Trace Recursion Depth", reinterpret_cast<int*>(&raytracingSettings.MaxTraceRecursionDepth), 1, MaxTraceRecursionDepth, "%d", ImGuiSliderFlags_AlwaysClamp);

						isChanged |= ImGui::SliderInt("Samples Per Pixel", reinterpret_cast<int*>(&raytracingSettings.SamplesPerPixel), 1, static_cast<int>(MaxSamplesPerPixel), "%d", ImGuiSliderFlags_AlwaysClamp);

						ImGui::TreePop();
					}

					if (ImGui::TreeNodeEx("Post-Processing", ImGuiTreeNodeFlags_DefaultOpen)) {
						auto& postProcessingSetttings = GraphicsSettings.PostProcessing;

						{
							const auto isAvailable = m_NRD->IsAvailable();

							const ImGuiEx::ScopedEnablement scopedEnablement(isAvailable);

							if (ImGui::TreeNodeEx("Raytracing Denoising", ImGuiTreeNodeFlags_DefaultOpen)) {
								auto& raytracingDenoisingSettings = postProcessingSetttings.RaytracingDenoising;

								auto isEnabled = isAvailable && raytracingDenoisingSettings.IsEnabled;

								{
									ImGui::PushID("Enable Raytracing Denoising");

									if (ImGui::Checkbox("Enable", &isEnabled)) {
										raytracingDenoisingSettings.IsEnabled = isEnabled;

										isChanged = true;
									}

									ImGui::PopID();
								}

								isChanged |= isEnabled && ImGui::Checkbox("Validation Layer", &raytracingDenoisingSettings.IsValidationLayerEnabled);

								isChanged |= isEnabled && ImGui::SliderFloat("Split Screen", &raytracingDenoisingSettings.SplitScreen, 0, 1, "%.2f", ImGuiSliderFlags_AlwaysClamp);

								ImGui::TreePop();
							}
						}

						isChanged |= ImGui::Checkbox("Temporal Anti-Aliasing", &postProcessingSetttings.IsTemporalAntiAliasingEnabled);

						if (ImGui::TreeNodeEx("Bloom", ImGuiTreeNodeFlags_DefaultOpen)) {
							auto& bloomSettings = postProcessingSetttings.Bloom;

							{
								ImGui::PushID("Enable Bloom");

								isChanged |= ImGui::Checkbox("Enable", &bloomSettings.IsEnabled);

								ImGui::PopID();
							}

							if (bloomSettings.IsEnabled) {
								isChanged |= ImGui::SliderFloat("Threshold", &bloomSettings.Threshold, 0, 1, "%.2f", ImGuiSliderFlags_AlwaysClamp);

								isChanged |= ImGui::SliderFloat("Blur size", &bloomSettings.BlurSize, 1, BloomMaxBlurSize, "%.2f", ImGuiSliderFlags_AlwaysClamp);
							}

							ImGui::TreePop();
						}

						ImGui::TreePop();
					}

					ImGui::TreePop();

					if (isChanged) ignore = GraphicsSettings.Save();
				}

				if (ImGui::TreeNode("UI")) {
					auto isChanged = false;

					isChanged |= ImGui::Checkbox("Show on Startup", &UISettings.ShowOnStartup);

					isChanged |= ImGui::SliderFloat("Window Opacity", &UISettings.WindowOpacity, 0, 1, "%.2f", ImGuiSliderFlags_AlwaysClamp);

					ImGui::TreePop();

					if (isChanged) ignore = UISettings.Save();
				}

				if (ImGui::TreeNode("Controls")) {
					auto isChanged = false;

					if (ImGui::TreeNodeEx("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
						auto& cameraSettings = ControlsSettings.Camera;

						if (ImGui::TreeNodeEx("Speed", ImGuiTreeNodeFlags_DefaultOpen)) {
							auto& speedSettings = cameraSettings.Speed;

							isChanged |= ImGui::SliderFloat("Translation", &speedSettings.Translation, CameraMinTranslationSpeed, CameraMaxTranslationSpeed, "%.1f", ImGuiSliderFlags_AlwaysClamp);

							isChanged |= ImGui::SliderFloat("Rotation", &speedSettings.Rotation, CameraMinRotationSpeed, CameraMaxRotationSpeed, "%.2f", ImGuiSliderFlags_AlwaysClamp);

							ImGui::TreePop();
						}

						ImGui::TreePop();
					}

					ImGui::TreePop();

					if (isChanged) ignore = ControlsSettings.Save();
				}
			}

			ImGui::End();
		}

		{
			const auto PopupModal = [&](LPCSTR name, const auto& lambda) {
				if (name == openPopupModalName) ImGui::OpenPopup(name);

				ImGui::SetNextWindowPos(viewport.GetWorkCenter(), ImGuiCond_Always, { 0.5f, 0.5f });
				ImGui::SetNextWindowSize({});

				if (ImGui::BeginPopupModal(name)) {
					lambda();

					ImGui::Separator();

					{
						constexpr auto Text = "OK";
						ImGuiEx::AlignForWidth(ImGui::CalcTextSize(Text).x);
						if (ImGui::Button(Text)) ImGui::CloseCurrentPopup();
						ImGui::SetItemDefaultFocus();
					}

					ImGui::EndPopup();
				}
			};

			PopupModal(
				"Controls",
				[] {
					{
						const auto AddContents = [](LPCSTR treeLabel, LPCSTR tableID, const initializer_list<pair<LPCSTR, LPCSTR>>& list) {
							if (ImGui::TreeNodeEx(treeLabel, ImGuiTreeNodeFlags_DefaultOpen)) {
								if (ImGui::BeginTable(tableID, 2, ImGuiTableFlags_Borders)) {
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
								{ "Menu", "Show/hide UI" },
								{ "X (hold)", "Show window switcher if UI visible" },
								{ "View", "Reset camera" },
								{ "LS (rotate)", "Move" },
								{ "LT (hold)", "Move slower" },
								{ "RT (hold)", "Move faster" },
								{ "RS (rotate)", "Look around" },
								{ "A", "Run/pause physics simulation" },
								{ "B", "Toggle gravity of Earth" },
								{ "Y", "Toggle gravity of the star" }
							}
						);

						AddContents(
							"Keyboard",
							"##Keyboard",
							{
								{ "Alt + Enter", "Toggle between windowed/borderless & fullscreen modes" },
								{ "Esc", "Show/hide UI" },
								{ "Ctrl + Tab (hold)", "Show window switcher if UI visible" },
								{ "Home", "Reset camera" },
								{ "W A S D", "Move" },
								{ "Left Ctrl (hold)", "Move slower" },
								{ "Left Shift (hold)", "Move faster" },
								{ "Space", "Run/pause physics simulation" },
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
				}
			);

			PopupModal(
				"About",
				[] {
					{
						ImGui::Text("© Hydr10n. All rights reserved.");

						if (constexpr auto URL = "https://github.com/Hydr10n/DirectX-Physically-Based-Raytracer";
							ImGuiEx::Hyperlink("GitHub repository", URL)) {
							ShellExecuteA(nullptr, "open", URL, nullptr, nullptr, SW_SHOW);
						}
					}
				}
			);
		}

		ImGui::Render();
		ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), m_deviceResources->GetCommandList());
	}
};

App::App(const shared_ptr<WindowModeHelper>& windowModeHelper) : m_impl(make_unique<Impl>(windowModeHelper)) {}

App::~App() = default;

SIZE App::GetOutputSize() const noexcept { return m_impl->GetOutputSize(); }

void App::Tick() { m_impl->Tick(); }

void App::OnWindowSizeChanged() { m_impl->OnWindowSizeChanged(); }

void App::OnDisplayChanged() { m_impl->OnDisplayChanged(); }

void App::OnResuming() { m_impl->OnResuming(); }

void App::OnSuspending() { m_impl->OnSuspending(); }

void App::OnActivated() { return m_impl->OnActivated(); }

void App::OnDeactivated() { return m_impl->OnDeactivated(); }
