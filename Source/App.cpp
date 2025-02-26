module;

#include <set>

#include "directx/d3dx12.h"

#include <shellapi.h>

#include "pix.h"

#include "directxtk12/CommonStates.h"
#include "directxtk12/DirectXHelpers.h"
#include "directxtk12/GamePad.h"
#include "directxtk12/Keyboard.h"
#include "directxtk12/Mouse.h"
#include "directxtk12/ResourceUploadBatch.h"
#include "directxtk12/SimpleMath.h"
#include "directxtk12/SpriteBatch.h"

#include "nvapi.h"

#include "Rtxdi/ImportanceSamplingContext.h"

#include "sl_helpers.h"
#include "sl_dlss_d.h"
#include "sl_dlss_g.h"

#include "NRD.h"

#include "xess/xess.h"

#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"
#include "ImGuiEx.h"

#include "MyAppData.h"

module App;

import Camera;
import CommandList;
import CommonShaderData;
import DescriptorHeap;
import DeviceResources;
import ErrorHelpers;
import GBufferGeneration;
import GPUBuffer;
import HaltonSampler;
import LightPreparation;
import Material;
import MyScene;
import PostProcessing.Bloom;
import PostProcessing.DenoisedComposition;
import PostProcessing.MipmapGeneration;
import Raytracing;
import RTXDI;
import RTXDIResources;
import RTXGI;
import SharedData;
import StepTimer;
import StringConverters;
import Texture;
import ThreadHelpers;

using namespace D3D12MA;
using namespace DirectX;
using namespace DirectX::SimpleMath;
using namespace DisplayHelpers;
using namespace DX;
using namespace ErrorHelpers;
using namespace PostProcessing;
using namespace rtxdi;
using namespace SharedData;
using namespace std;
using namespace ThreadHelpers;
using namespace WindowHelpers;

using GamepadButtonState = GamePad::ButtonStateTracker::ButtonState;
using Key = Keyboard::Keys;

namespace {
	constexpr auto& g_graphicsSettings = MyAppData::Settings::Graphics;
	constexpr auto& g_UISettings = MyAppData::Settings::UI;
	constexpr auto& g_controlsSettings = MyAppData::Settings::Controls;
}

#define MAKE_NAME(Name) static constexpr LPCSTR Name = #Name;
#define MAKE_NAMEW(Name) static constexpr LPCWSTR Name = L#Name;

struct App::Impl : IDeviceNotify {
	Impl(WindowModeHelper& windowModeHelper) noexcept(false) : m_windowModeHelper(windowModeHelper) {
		{
			ImGui::CreateContext();

			ImGui::StyleColorsDark();

			auto& IO = ImGui::GetIO();

			IO.IniFilename = IO.LogFilename = nullptr;

			IO.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
			IO.BackendFlags |= ImGuiBackendFlags_HasGamepad;

			ImGui_ImplWin32_Init(m_windowModeHelper.hWnd);

			m_UIStates.IsVisible = g_UISettings.ShowOnStartup;
		}

		{
			m_deviceResources->RegisterDeviceNotify(this);

			m_deviceResources->CreateDeviceResources();
			CreateDeviceDependentResources();

			m_deviceResources->SetWindow(windowModeHelper.hWnd, windowModeHelper.GetResolution());
			CreateWindowSizeDependentResources();

			m_deviceResources->EnableVSync(g_graphicsSettings.IsVSyncEnabled);
			m_deviceResources->RequestHDR(g_graphicsSettings.IsHDREnabled);
		}

		m_inputDevices.Mouse->SetWindow(windowModeHelper.hWnd);

		windowModeHelper.SetFullscreenResolutionHandledByWindow(false);

		LoadScene();
	}

	~Impl() {
		m_deviceResources->WaitForGPU();

		{
			if (ImGui::GetIO().BackendRendererUserData != nullptr) {
				ImGui_ImplDX12_Shutdown();
			}

			ImGui_ImplWin32_Shutdown();

			ImGui::DestroyContext();
		}
	}

	SIZE GetOutputSize() const noexcept { return m_deviceResources->GetOutputSize(); }

	void Tick() {
		ignore = m_streamline->NewFrame();

		if (const auto pFuture = m_futures.find(FutureNames::Scene);
			pFuture != cend(m_futures) && pFuture->second.wait_for(0s) == future_status::ready) {
			m_futures.erase(pFuture);

			m_resetHistory = true;
		}

		{
			m_deviceResources->Prepare();

			m_stepTimer.Tick([&] { Update(); });

			Render();

			m_deviceResources->WaitForGPU();

			m_graphicsMemory->Commit(m_deviceResources->GetDeviceContext().CommandQueue);
		}

		erase_if(
			m_futures,
			[&](auto& future) {
				const auto ret = future.second.wait_for(0s) == future_status::deferred;
		if (ret) {
			future.second.get();
		}
		return ret;
			}
		);

		m_inputDevices.Mouse->EndOfInputFrame();

		{
			const scoped_lock lock(m_exceptionMutex);

			if (m_exception) {
				rethrow_exception(m_exception);
			}
		}
	}

	void OnWindowSizeChanged() {
		if (m_deviceResources->ResizeWindow(m_windowModeHelper.GetResolution())) {
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
		if (ImGui::GetIO().BackendRendererUserData != nullptr) {
			ImGui_ImplDX12_Shutdown();
		}

		m_scene.reset();

		m_GPUBuffers = {};

		m_textures = {};

		m_mipmapGeneration.reset();

		m_textureAlphaBlending.reset();

		m_textureCopy.reset();

		for (auto& toneMapping : m_toneMapping) {
			toneMapping.reset();
		}

		m_bloom.reset();

		m_XeSS.reset();

		m_denoisedComposition.reset();
		m_NRD.reset();

		m_streamline.reset();

		m_RTXDI.reset();
		m_lightPreparation.reset();
		m_RTXDIResources = {};

		m_raytracing.reset();

		m_SHARC.reset();

		m_GBufferGeneration.reset();

		m_graphicsMemory.reset();
	}

	void OnDeviceRestored() override {
		CreateDeviceDependentResources();

		CreateWindowSizeDependentResources();

		LoadScene();
	}

private:
	WindowModeHelper& m_windowModeHelper;

	unique_ptr<DeviceResources> m_deviceResources = make_unique<DeviceResources>(DeviceResources::CreationDesc{
		.MinFeatureLevel = D3D_FEATURE_LEVEL_12_1,
		.MinRaytracingTier = D3D12_RAYTRACING_TIER_1_1,
		.BackBufferFormat = DXGI_FORMAT_R10G10B10A2_UNORM,
		.DepthStencilBufferFormat = DXGI_FORMAT_UNKNOWN,
		.OptionFlags = DeviceResources::OptionFlags::DisableGPUTimeout | DeviceResources::OptionFlags::ReverseDepth
		});

	StepTimer m_stepTimer;

	unique_ptr<GraphicsMemory> m_graphicsMemory;

	const struct {
		unique_ptr<GamePad> Gamepad = make_unique<::GamePad>();
		unique_ptr<Keyboard> Keyboard = make_unique<::Keyboard>();
		unique_ptr<Mouse> Mouse = make_unique<::Mouse>();
	} m_inputDevices;

	struct {
		GamePad::ButtonStateTracker Gamepad;
		Keyboard::KeyboardStateTracker Keyboard;
		Mouse::ButtonStateTracker Mouse;
	} m_inputDeviceStateTrackers;

	bool m_isShaderExecutionReorderingAvailable{};

	bool m_resetHistory{};

	XMUINT2 m_renderSize{};

	HaltonSampler m_haltonSampler;

	struct FutureNames {
		MAKE_NAME(Scene);
	};
	unordered_map<string, future<void>> m_futures;

	exception_ptr m_exception;
	mutex m_exceptionMutex;

	unique_ptr<GBufferGeneration> m_GBufferGeneration;

	unique_ptr<SHARC> m_SHARC;

	unique_ptr<Raytracing> m_raytracing;

	RTXDIResources m_RTXDIResources;
	unique_ptr<LightPreparation> m_lightPreparation;
	unique_ptr<RTXDI> m_RTXDI;

	unique_ptr<Streamline> m_streamline;

	bool m_isReflexLowLatencyAvailable{}, m_isDLSSFrameGenerationEnabled{};

	sl::DLSSDOptions m_DLSSDOptions;

	unique_ptr<NRD> m_NRD;
	unique_ptr<DenoisedComposition> m_denoisedComposition;

	unique_ptr<XeSS> m_XeSS;

	unique_ptr<Bloom> m_bloom;

	unique_ptr<ToneMapPostProcess> m_toneMapping[ToneMapPostProcess::Operator_Max];

	unique_ptr<BasicPostProcess> m_textureCopy;

	unique_ptr<SpriteBatch> m_textureAlphaBlending;

	unique_ptr<MipmapGeneration> m_mipmapGeneration;

	static constexpr DXGI_FORMAT m_HDRTextureFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
	struct TextureNames {
		MAKE_NAMEW(Color);
		MAKE_NAMEW(Position);
		MAKE_NAMEW(FlatNormal);
		MAKE_NAMEW(PreviousGeometricNormal);
		MAKE_NAMEW(GeometricNormal);
		MAKE_NAMEW(PreviousLinearDepth);
		MAKE_NAMEW(LinearDepth);
		MAKE_NAMEW(NormalizedDepth);
		MAKE_NAMEW(MotionVector);
		MAKE_NAMEW(PreviousBaseColorMetalness);
		MAKE_NAMEW(BaseColorMetalness);
		MAKE_NAMEW(PreviousNormalRoughness);
		MAKE_NAMEW(NormalRoughness);
		MAKE_NAMEW(PreviousTransmission);
		MAKE_NAMEW(Transmission);
		MAKE_NAMEW(PreviousIOR);
		MAKE_NAMEW(IOR);
		MAKE_NAMEW(LightRadiance);
		MAKE_NAMEW(Radiance);
		MAKE_NAMEW(Diffuse);
		MAKE_NAMEW(Specular);
		MAKE_NAMEW(SpecularHitDistance);
		MAKE_NAMEW(DenoisedDiffuse);
		MAKE_NAMEW(DenoisedSpecular);
		MAKE_NAMEW(Validation);
	};
	unordered_map<wstring, shared_ptr<Texture>> m_textures;

	struct { unique_ptr<GPUBuffer> Camera, SceneData, InstanceData, ObjectData; } m_GPUBuffers;

	Camera m_camera;
	CameraController m_cameraController;

	unique_ptr<Scene> m_scene;

	struct { bool IsVisible, HasFocus = true, IsSettingsWindowOpen; } m_UIStates{};
	vector<unique_ptr<Descriptor>> m_ImGUIDescriptors;

	void CreateDeviceDependentResources() {
		const auto& deviceContext = m_deviceResources->GetDeviceContext();

		m_graphicsMemory = make_unique<GraphicsMemory>(deviceContext);

		{
			NVAPI_D3D12_RAYTRACING_THREAD_REORDERING_CAPS caps;
			m_isShaderExecutionReorderingAvailable = NvAPI_D3D12_GetRaytracingCaps(deviceContext, NVAPI_D3D12_RAYTRACING_CAPS_TYPE_THREAD_REORDERING, &caps, sizeof(caps)) == NVAPI_OK
				&& caps & NVAPI_D3D12_RAYTRACING_THREAD_REORDERING_CAP_STANDARD
				&& NvAPI_D3D12_SetNvShaderExtnSlotSpace(deviceContext, 1024, 0) == NVAPI_OK;
		}

		CreatePipelineStates();

		CreateConstantBuffers();
	}

	void CreateWindowSizeDependentResources() {
		const auto& deviceResourcesCreationDesc = m_deviceResources->GetCreationDesc();
		const auto& deviceContext = m_deviceResources->GetDeviceContext();

		const auto outputSize = GetOutputSize();

		{
			const auto CreateTexture = [&](LPCWSTR name, DXGI_FORMAT format, bool RTV = false, bool SRV = true, bool UAV = true, Color clearColor = {}) {
				Texture::CreationDesc creationDesc{
					.Format = format,
					.Width = static_cast<UINT>(outputSize.cx),
					.Height = static_cast<UINT>(outputSize.cy),
					.ClearColor = clearColor,
					.KeepInitialState = true
				};
				if (UAV) {
					creationDesc.AsUnorderedAccess();
				}
				if (RTV) {
					creationDesc.AsRenderTarget();
				}
				auto& texture = m_textures[name];
				texture = make_shared<Texture>(deviceContext, creationDesc);
				texture->GetNative()->SetName(name);
				if (SRV) {
					texture->CreateSRV();
				}
				if (UAV) {
					texture->CreateUAV();
				}
				if (RTV) {
					texture->CreateRTV();
				}
			};

			CreateTexture(TextureNames::Color, m_HDRTextureFormat, true);
			CreateTexture(TextureNames::Position, DXGI_FORMAT_R32G32B32A32_FLOAT);
			CreateTexture(TextureNames::FlatNormal, DXGI_FORMAT_R16G16_SNORM);
			CreateTexture(TextureNames::PreviousGeometricNormal, DXGI_FORMAT_R16G16_SNORM);
			CreateTexture(TextureNames::GeometricNormal, DXGI_FORMAT_R16G16_SNORM);
			CreateTexture(TextureNames::PreviousLinearDepth, DXGI_FORMAT_R32_FLOAT, true, true, true, Color(numeric_limits<float>::infinity(), 0, 0));
			CreateTexture(TextureNames::LinearDepth, DXGI_FORMAT_R32_FLOAT, true, true, true, Color(numeric_limits<float>::infinity(), 0, 0));
			CreateTexture(TextureNames::NormalizedDepth, DXGI_FORMAT_R32_FLOAT);
			CreateTexture(TextureNames::MotionVector, DXGI_FORMAT_R16G16B16A16_FLOAT);
			CreateTexture(TextureNames::PreviousBaseColorMetalness, DXGI_FORMAT_R8G8B8A8_UNORM);
			CreateTexture(TextureNames::BaseColorMetalness, DXGI_FORMAT_R8G8B8A8_UNORM);
			CreateTexture(TextureNames::PreviousNormalRoughness, DXGI_FORMAT_R16G16B16A16_SNORM);
			CreateTexture(TextureNames::NormalRoughness, DXGI_FORMAT_R16G16B16A16_SNORM);
			CreateTexture(TextureNames::PreviousTransmission, DXGI_FORMAT_R8_UNORM);
			CreateTexture(TextureNames::Transmission, DXGI_FORMAT_R8_UNORM);
			CreateTexture(TextureNames::PreviousIOR, DXGI_FORMAT_R16_FLOAT);
			CreateTexture(TextureNames::IOR, DXGI_FORMAT_R16_FLOAT);
			CreateTexture(TextureNames::LightRadiance, m_HDRTextureFormat, true);
			CreateTexture(TextureNames::Radiance, m_HDRTextureFormat, true);

			{
				m_NRD = make_unique<NRD>(
					m_deviceResources->GetCommandList(),
					outputSize.cx, outputSize.cy,
					deviceResourcesCreationDesc.BackBufferCount,
					initializer_list<nrd::DenoiserDesc>{
						{ static_cast<nrd::Identifier>(Denoiser::NRDReBLUR), nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR },
						{ static_cast<nrd::Identifier>(Denoiser::NRDReLAX), nrd::Denoiser::RELAX_DIFFUSE_SPECULAR }
				}
				);

				const auto
					isNRDAvailable = m_NRD->IsAvailable(),
					isDLSSRayReconstructionAvailable = m_streamline->IsAvailable(sl::kFeatureDLSS_RR);
				if (isNRDAvailable) {
					CreateTexture(TextureNames::DenoisedDiffuse, DXGI_FORMAT_R16G16B16A16_FLOAT, false);
					CreateTexture(TextureNames::DenoisedSpecular, DXGI_FORMAT_R16G16B16A16_FLOAT, false);
					CreateTexture(TextureNames::Validation, DXGI_FORMAT_R8G8B8A8_UNORM);
				}
				if (isNRDAvailable || isDLSSRayReconstructionAvailable) {
					CreateTexture(TextureNames::Diffuse, DXGI_FORMAT_R16G16B16A16_FLOAT, true, false);
					CreateTexture(TextureNames::Specular, DXGI_FORMAT_R16G16B16A16_FLOAT, true, false);
				}
				if (isDLSSRayReconstructionAvailable) {
					CreateTexture(TextureNames::SpecularHitDistance, DXGI_FORMAT_R16_FLOAT, true, false);
				}

				SelectDenoiser();
			}

			m_XeSS = make_unique<XeSS>(deviceContext, xess_2d_t{ static_cast<uint32_t>(outputSize.cx), static_cast<uint32_t>(outputSize.cy) }, XESS_INIT_FLAG_INVERTED_DEPTH | XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE);

			SelectSuperResolutionUpscaler();
			SetSuperResolutionOptions();
		}

		m_cameraController.SetLens(XMConvertToRadians(g_graphicsSettings.Camera.HorizontalFieldOfView), static_cast<float>(outputSize.cx) / static_cast<float>(outputSize.cy));

		{
			const auto& IO = ImGui::GetIO();

			if (IO.BackendRendererUserData != nullptr) {
				ImGui_ImplDX12_Shutdown();
			}

			ImGui_ImplDX12_InitInfo initInfo;
			initInfo.Device = deviceContext;
			initInfo.CommandQueue = deviceContext.CommandQueue;
			initInfo.NumFramesInFlight = deviceResourcesCreationDesc.BackBufferCount;
			initInfo.RTVFormat = deviceResourcesCreationDesc.BackBufferFormat;
			initInfo.UserData = this;
			initInfo.SrvDescriptorHeap = *deviceContext.ResourceDescriptorHeap;
			initInfo.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* initInfo, D3D12_CPU_DESCRIPTOR_HANDLE* CPUHandle, D3D12_GPU_DESCRIPTOR_HANDLE* GPUHandle) {
				const auto app = static_cast<Impl*>(initInfo->UserData);
				auto descriptor = app->m_deviceResources->GetDeviceContext().ResourceDescriptorHeap->Allocate();
				*CPUHandle = *descriptor;
				*GPUHandle = *descriptor;
				app->m_ImGUIDescriptors.emplace_back(move(descriptor));
			};
			initInfo.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo* initInfo, D3D12_CPU_DESCRIPTOR_HANDLE CPUHandle, D3D12_GPU_DESCRIPTOR_HANDLE) {
				auto& descriptors = static_cast<Impl*>(initInfo->UserData)->m_ImGUIDescriptors;
				if (const auto pDescriptor = ranges::find_if(descriptors, [&](const unique_ptr<Descriptor>& descriptor) { return descriptor->GetCPUHandle().ptr == CPUHandle.ptr; });
					pDescriptor != cend(descriptors)) {
					descriptors.erase(pDescriptor);
				}
			};
			ImGui_ImplDX12_Init(&initInfo);

			IO.Fonts->Clear();
			IO.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/segoeui.ttf", static_cast<float>(outputSize.cy) * 0.022f);
		}
	}

	void Update() {
		const auto isPCLAvailable = m_streamline->IsAvailable(sl::kFeaturePCL);

		if (isPCLAvailable) {
			ignore = m_streamline->SetPCLMarker(sl::PCLMarker::eSimulationStart);
		}

		{
			m_camera.IsNormalizedDepthReversed = true;
			m_camera.PreviousPosition = m_cameraController.GetPosition();
			m_camera.PreviousWorldToView = m_cameraController.GetWorldToView();
			m_camera.PreviousViewToProjection = m_cameraController.GetViewToProjection();
			m_camera.PreviousWorldToProjection = m_cameraController.GetWorldToProjection();
			m_camera.PreviousProjectionToView = m_cameraController.GetProjectionToView();
			m_camera.PreviousViewToWorld = m_cameraController.GetViewToWorld();

			ProcessInput();

			m_camera.Position = m_cameraController.GetPosition();
			m_camera.RightDirection = m_cameraController.GetRightDirection();
			m_camera.UpDirection = m_cameraController.GetUpDirection();
			m_camera.ForwardDirection = m_cameraController.GetForwardDirection();
			m_camera.NearDepth = m_cameraController.GetNearDepth();
			m_camera.FarDepth = m_cameraController.GetFarDepth();
			m_camera.Jitter = g_graphicsSettings.Camera.IsJitterEnabled ? m_haltonSampler.GetNext2D() - Vector2(0.5f) : XMFLOAT2();
			m_camera.WorldToProjection = m_cameraController.GetWorldToProjection();
			m_camera.ProjectionToView = m_cameraController.GetProjectionToView();
			m_camera.ViewToWorld = m_cameraController.GetViewToWorld();

			m_deviceResources->GetCommandList().Copy(*m_GPUBuffers.Camera, initializer_list{ m_camera });
		}

		if (IsSceneReady()) {
			UpdateScene();
		}

		if (isPCLAvailable) {
			ignore = m_streamline->SetPCLMarker(sl::PCLMarker::eSimulationEnd);
		}
	}

	void Render() {
		const auto isSceneReady = IsSceneReady();

		if (IsDLSSFrameGenerationEnabled()) {
			if (!m_isDLSSFrameGenerationEnabled && isSceneReady) {
				SetDLSSFrameGenerationOptions(true);
			}
			else if (m_isDLSSFrameGenerationEnabled && !isSceneReady) {
				SetDLSSFrameGenerationOptions(false);
			}
		}
		else if (m_isDLSSFrameGenerationEnabled) {
			SetDLSSFrameGenerationOptions(false);
		}

		auto& commandList = m_deviceResources->GetCommandList();

		const auto isPCLAvailable = m_streamline->IsAvailable(sl::kFeaturePCL);

		if (isPCLAvailable) {
			ignore = m_streamline->SetPCLMarker(sl::PCLMarker::eRenderSubmitStart);
		}

		auto& backBuffer = m_deviceResources->GetBackBuffer();
		commandList.SetRenderTarget(backBuffer);
		commandList.Clear(backBuffer);

		const auto viewport = m_deviceResources->GetScreenViewport();
		const auto scissorRect = m_deviceResources->GetScissorRect();
		commandList->RSSetViewports(1, &viewport);
		commandList->RSSetScissorRects(1, &scissorRect);

		{
			const ScopedPixEvent scopedPixEvent(commandList, PIX_COLOR_DEFAULT, L"Render");

			if (isSceneReady) {
				if (m_resetHistory) {
					ResetHistory();
				}

				if (!m_scene->IsStatic()) {
					m_scene->CreateAccelerationStructures(commandList);
					commandList.CompactAccelerationStructures();
				}

				m_scene->CollectGarbage();

				RenderScene();

				PostProcessGraphics();

				m_resetHistory = false;

				{
					swap(m_textures.at(TextureNames::PreviousGeometricNormal), m_textures.at(TextureNames::GeometricNormal));
					swap(m_textures.at(TextureNames::PreviousLinearDepth), m_textures.at(TextureNames::LinearDepth));
					swap(m_textures.at(TextureNames::PreviousBaseColorMetalness), m_textures.at(TextureNames::BaseColorMetalness));
					swap(m_textures.at(TextureNames::PreviousNormalRoughness), m_textures.at(TextureNames::NormalRoughness));
					swap(m_textures.at(TextureNames::PreviousTransmission), m_textures.at(TextureNames::Transmission));
					swap(m_textures.at(TextureNames::PreviousIOR), m_textures.at(TextureNames::IOR));
				}
			}

			if (m_UIStates.IsVisible) {
				RenderUI();
			}
		}

		if (isPCLAvailable) {
			ignore = m_streamline->SetPCLMarker(sl::PCLMarker::eRenderSubmitEnd);

			ignore = m_streamline->SetPCLMarker(sl::PCLMarker::ePresentStart);
		}

		m_deviceResources->Present();

		if (isPCLAvailable) {
			ignore = m_streamline->SetPCLMarker(sl::PCLMarker::ePresentEnd);
		}
	}

	void OnRenderSizeChanged() {
		const auto outputSize = GetOutputSize();

		m_resetHistory = true;

		m_haltonSampler = HaltonSampler(static_cast<uint32_t>(ceil(8 * (static_cast<float>(outputSize.cx) / static_cast<float>(m_renderSize.x)) * (static_cast<float>(outputSize.cy) / static_cast<float>(m_renderSize.y)))));

		{
			m_RTXDIResources.Context = make_unique<ImportanceSamplingContext>(ImportanceSamplingContext_StaticParameters{ .renderWidth = m_renderSize.x, .renderHeight = m_renderSize.y });

			{
				auto& commandList = m_deviceResources->GetCommandList();
				commandList.Begin();

				m_RTXDIResources.CreateRenderSizeDependentResources(commandList);

				commandList.End();
			}
		}
	}

	void ResetHistory() {
		auto& commandList = m_deviceResources->GetCommandList();

		commandList.Clear(*m_textures.at(TextureNames::PreviousLinearDepth));

		if (g_graphicsSettings.Raytracing.RTXGI.Technique == RTXGITechnique::SHARC) {
			commandList.Clear(*m_SHARC->GPUBuffers.HashEntries);
			commandList.Clear(*m_SHARC->GPUBuffers.HashCopyOffset);
			commandList.Clear(*m_SHARC->GPUBuffers.PreviousVoxelData);
		}

		m_haltonSampler.Reset();
	}

	bool IsSceneLoading() const { return m_futures.contains(FutureNames::Scene); }
	bool IsSceneReady() const { return !IsSceneLoading() && m_scene; }

	void LoadScene() {
		m_futures[FutureNames::Scene] = StartDetachedFuture([&] {
			try {
			m_scene = make_unique<MyScene>(m_deviceResources->GetDeviceContext());
			m_scene->Load(MySceneDesc());

			OnSceneLoaded();
		}
		catch (...) {
			const scoped_lock lock(m_exceptionMutex);

			if (!m_exception) {
				m_exception = current_exception();
			}
		}
			});
	}

	void OnSceneLoaded() {
		CreateStructuredBuffers();

		ResetCamera();

		PrepareLightResources();
	}

	void CreatePipelineStates() {
		const auto& deviceContext = m_deviceResources->GetDeviceContext();

		m_GBufferGeneration = make_unique<GBufferGeneration>(deviceContext);

		{
			m_SHARC = make_unique<SHARC>(deviceContext);
			m_SHARC->Configure();
		}

		{
			CommandList commandList(deviceContext);
			commandList.Begin();

			m_raytracing = make_unique<Raytracing>(commandList);

			commandList.End();
		}

		m_lightPreparation = make_unique<LightPreparation>(deviceContext);
		m_RTXDI = make_unique<RTXDI>(deviceContext);

		{
			ignore = slSetD3DDevice(deviceContext);

			m_streamline = make_unique<Streamline>(m_deviceResources->GetCommandList(), 0);

			if (m_streamline->IsAvailable(sl::kFeatureReflex)) {
				sl::ReflexState state;
				ignore = slReflexGetState(state);
				m_isReflexLowLatencyAvailable = state.lowLatencyAvailable;

				SetReflexOptions();
			}
		}

		CreatePostProcessing();
	}

	void CreatePostProcessing() {
		const auto& deviceContext = m_deviceResources->GetDeviceContext();

		m_denoisedComposition = make_unique<DenoisedComposition>(deviceContext);

		m_bloom = make_unique<Bloom>(deviceContext);

		{
			const RenderTargetState renderTargetState(m_HDRTextureFormat, DXGI_FORMAT_UNKNOWN);

			for (const auto Operator : {
				ToneMapPostProcess::Saturate,
				ToneMapPostProcess::Reinhard,
				ToneMapPostProcess::ACESFilmic,
				ToneMapPostProcess::Operator_Max
				}) {
				m_toneMapping[Operator - 1] = make_unique<ToneMapPostProcess>(
					deviceContext,
					renderTargetState,
					Operator == ToneMapPostProcess::Operator_Max ? ToneMapPostProcess::None : Operator,
					Operator == ToneMapPostProcess::Operator_Max ? ToneMapPostProcess::ST2084 : ToneMapPostProcess::SRGB
					);
			}
		}

		{
			const RenderTargetState renderTargetState(m_deviceResources->GetCreationDesc().BackBufferFormat, DXGI_FORMAT_UNKNOWN);

			m_textureCopy = make_unique<BasicPostProcess>(deviceContext, renderTargetState, BasicPostProcess::Copy);

			{
				ResourceUploadBatch resourceUploadBatch(deviceContext);
				resourceUploadBatch.Begin();

				m_textureAlphaBlending = make_unique<SpriteBatch>(deviceContext, resourceUploadBatch, SpriteBatchPipelineStateDescription(renderTargetState, &CommonStates::NonPremultiplied));

				resourceUploadBatch.End(deviceContext.CommandQueue).get();
			}
		}

		m_mipmapGeneration = make_unique<MipmapGeneration>(deviceContext);
	}

	void CreateConstantBuffers() {
		const auto CreateBuffer = [&]<typename T>(auto & buffer) {
			buffer = GPUBuffer::CreateConstant<T, false>(m_deviceResources->GetDeviceContext());
		};
		CreateBuffer.operator() < Camera > (m_GPUBuffers.Camera);
		CreateBuffer.operator() < SceneData > (m_GPUBuffers.SceneData);
	}

	void CreateStructuredBuffers() {
		const auto CreateBuffer = [&]<typename T>(T, auto & buffer, uint64_t capacity) {
			buffer = GPUBuffer::CreateDefault<T>(m_deviceResources->GetDeviceContext(), capacity);
		};
		if (const auto instanceCount = size(m_scene->GetInstanceData())) {
			CreateBuffer(InstanceData(), m_GPUBuffers.InstanceData, instanceCount);
		}
		if (const auto objectCount = m_scene->GetObjectCount()) {
			CreateBuffer(ObjectData(), m_GPUBuffers.ObjectData, objectCount);
		}
	}

	void ProcessInput() {
		auto& gamepadStateTracker = m_inputDeviceStateTrackers.Gamepad;
		if (const auto gamepadState = m_inputDevices.Gamepad->GetState(0); gamepadState.IsConnected()) {
			gamepadStateTracker.Update(gamepadState);
		}
		else {
			gamepadStateTracker.Reset();
		}

		auto& keyboardStateTracker = m_inputDeviceStateTrackers.Keyboard;
		keyboardStateTracker.Update(m_inputDevices.Keyboard->GetState());

		const auto mouseState = m_inputDevices.Mouse->GetState();
		auto& mouseStateTracker = m_inputDeviceStateTrackers.Mouse;
		mouseStateTracker.Update(mouseState);
		m_inputDevices.Mouse->ResetScrollWheelValue();

		const auto isUIVisible = m_UIStates.IsVisible;

		{
			if (gamepadStateTracker.menu == GamepadButtonState::PRESSED) {
				m_UIStates.IsVisible = !m_UIStates.IsVisible;
			}
			if (keyboardStateTracker.IsKeyPressed(Key::Escape)) {
				m_UIStates.IsVisible = !m_UIStates.IsVisible;
			}
		}

		const auto isSceneReady = IsSceneReady();

		if (m_UIStates.IsVisible) {
			if (!isUIVisible) {
				m_UIStates.HasFocus = true;

				if (const auto e = ImGuiEx::FindLatestInputEvent(ImGui::GetCurrentContext(), ImGuiInputEventType_MousePos)) {
					auto& queue = ImGui::GetCurrentContext()->InputEventsQueue;
					queue[0] = *e;
					queue.shrink(1);
				}
			}

			auto& IO = ImGui::GetIO();
			if (IO.WantCaptureKeyboard) {
				m_UIStates.HasFocus = true;
			}
			if (IO.WantCaptureMouse && !(IO.ConfigFlags & ImGuiConfigFlags_NoMouse)) {
				m_UIStates.HasFocus = true;
			}
			else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && isSceneReady) {
				m_UIStates.HasFocus = false;
			}
			if (m_UIStates.HasFocus) {
				IO.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;

				if (mouseState.positionMode == Mouse::MODE_RELATIVE) {
					m_inputDevices.Mouse->SetMode(Mouse::MODE_ABSOLUTE);
					while (ShowCursor(TRUE) < 0) {}
				}
			}
			else {
				IO.ConfigFlags |= ImGuiConfigFlags_NoMouse;
			}
		}

		if (isSceneReady && (!m_UIStates.IsVisible || !m_UIStates.HasFocus)) {
			if (mouseState.positionMode == Mouse::MODE_ABSOLUTE) {
				m_inputDevices.Mouse->SetMode(Mouse::MODE_RELATIVE);
				while (ShowCursor(FALSE) >= 0) {}
			}

			UpdateCamera();
		}
	}

	void ResetCamera() {
		m_cameraController.SetPosition(m_scene->Camera.Position);
		m_cameraController.SetRotation(m_scene->Camera.Rotation);

		m_resetHistory = true;
	}

	void UpdateCamera() {
		const auto elapsedSeconds = static_cast<float>(m_stepTimer.GetElapsedSeconds());

		const auto gamepadState = m_inputDeviceStateTrackers.Gamepad.GetLastState();
		const auto keyboardState = m_inputDeviceStateTrackers.Keyboard.GetLastState();
		const auto mouseState = m_inputDeviceStateTrackers.Mouse.GetLastState();

		Vector3 displacement;
		float yaw = 0, pitch = 0;

		auto& speedSettings = g_controlsSettings.Camera.Speed;

		if (gamepadState.IsConnected()) {
			if (m_inputDeviceStateTrackers.Gamepad.view == GamepadButtonState::PRESSED) {
				ResetCamera();
			}

			int movementSpeedIncrement = 0;
			if (gamepadState.IsDPadUpPressed()) {
				movementSpeedIncrement++;
			}
			if (gamepadState.IsDPadDownPressed()) {
				movementSpeedIncrement--;
			}
			if (movementSpeedIncrement) {
				speedSettings.Movement = clamp(speedSettings.Movement + elapsedSeconds * static_cast<float>(movementSpeedIncrement) * 12, 0.0f, speedSettings.MaxMovement);
			}

			const auto movementSpeed = elapsedSeconds * speedSettings.Movement;
			displacement.x += gamepadState.thumbSticks.leftX * movementSpeed;
			displacement.z += gamepadState.thumbSticks.leftY * movementSpeed;

			const auto rotationSpeed = elapsedSeconds * XM_2PI * speedSettings.Rotation;
			yaw += gamepadState.thumbSticks.rightX * rotationSpeed;
			pitch += gamepadState.thumbSticks.rightY * rotationSpeed;
		}

		if (mouseState.positionMode == Mouse::MODE_RELATIVE) {
			if (m_inputDeviceStateTrackers.Keyboard.IsKeyPressed(Key::Home)) {
				ResetCamera();
			}

			if (mouseState.scrollWheelValue) {
				speedSettings.Movement = clamp(speedSettings.Movement + static_cast<float>(mouseState.scrollWheelValue) * 0.008f, 0.0f, speedSettings.MaxMovement);
			}

			const auto movementSpeed = elapsedSeconds * speedSettings.Movement;
			if (keyboardState.A) {
				displacement.x -= movementSpeed;
			}
			if (keyboardState.D) {
				displacement.x += movementSpeed;
			}
			if (keyboardState.W) {
				displacement.z += movementSpeed;
			}
			if (keyboardState.S) {
				displacement.z -= movementSpeed;
			}

			const auto rotationSpeed = 4e-4f * XM_2PI * speedSettings.Rotation;
			yaw += static_cast<float>(mouseState.x) * rotationSpeed;
			pitch += static_cast<float>(-mouseState.y) * rotationSpeed;
		}

		if (pitch == 0) {
			if (displacement == Vector3() && yaw == 0) {
				return;
			}
		}
		else if (const auto angle = XM_PIDIV2 - abs(-m_cameraController.GetRotation().ToEuler().x + pitch);
			angle <= 0) {
			pitch = copysign(max(0.0f, angle - 0.1f), pitch);
		}

		m_cameraController.Translate(m_cameraController.GetNormalizedRightDirection() * displacement.x + m_cameraController.GetNormalizedUpDirection() * displacement.y + m_cameraController.GetNormalizedForwardDirection() * displacement.z);
		m_cameraController.Rotate(yaw, pitch);
	}

	void UpdateScene() {
		m_scene->Tick(m_stepTimer.GetElapsedSeconds(), m_inputDeviceStateTrackers.Gamepad, m_inputDeviceStateTrackers.Keyboard, m_inputDeviceStateTrackers.Mouse);

		auto& commandList = m_deviceResources->GetCommandList();

		{
			SceneData sceneData{
				.IsStatic = m_scene->IsStatic(),
				.EnvironmentLightColor = m_scene->EnvironmentLight.Color
			};
			XMStoreFloat3x4(&sceneData.EnvironmentLightTransform, Matrix::CreateFromQuaternion(m_scene->EnvironmentLight.Rotation));
			if (m_scene->EnvironmentLight.Texture) {
				sceneData.IsEnvironmentLightTextureCubeMap = m_scene->EnvironmentLight.Texture->IsCubeMap();
				sceneData.EnvironmentLightTextureDescriptor = m_scene->EnvironmentLight.Texture->GetSRVDescriptor();
			}
			commandList.Copy(*m_GPUBuffers.SceneData, initializer_list{ sceneData });
		}

		vector<InstanceData> instanceData(size(m_scene->GetInstanceData()));
		vector<ObjectData> objectData(m_scene->GetObjectCount());
		for (uint32_t instanceIndex = 0; const auto & renderObject : m_scene->RenderObjects) {
			const auto& _instanceData = m_scene->GetInstanceData()[instanceIndex];
			instanceData[instanceIndex++] = {
				.FirstGeometryIndex = _instanceData.FirstGeometryIndex,
				.PreviousObjectToWorld = _instanceData.PreviousObjectToWorld,
				.ObjectToWorld = _instanceData.ObjectToWorld
			};

			const auto& mesh = renderObject.Mesh;

			auto& _objectData = objectData[_instanceData.FirstGeometryIndex];

			_objectData.VertexDesc = mesh->GetVertexDesc();

			_objectData.Material = renderObject.Material;

			_objectData.MeshDescriptors = {
				.Vertices = mesh->Vertices->GetSRVDescriptor(BufferSRVType::Raw),
				.Indices = mesh->Indices->GetSRVDescriptor(BufferSRVType::Typed)
			};

			for (uint32_t i = 0;
				const auto & texture : renderObject.Textures) {
				if (texture) {
					_objectData.TextureMapInfoArray[i] = {
						.Descriptor = texture->GetSRVDescriptor().GetIndex()
					};
				}
				i++;
			}
		}
		if (m_GPUBuffers.InstanceData) {
			commandList.Copy(*m_GPUBuffers.InstanceData, instanceData);
		}
		if (m_GPUBuffers.ObjectData) {
			commandList.Copy(*m_GPUBuffers.ObjectData, objectData);
		}
	}

	void PrepareLightResources() {
		m_lightPreparation->SetScene(m_scene.get());
		if (const auto emissiveTriangleCount = m_lightPreparation->GetEmissiveTriangleCount()) {
			const auto& deviceContext = m_deviceResources->GetDeviceContext();

			m_RTXDIResources.CreateLightResources(deviceContext, emissiveTriangleCount, m_scene->GetObjectCount());

			{
				CommandList commandList(deviceContext);
				commandList.Begin();

				m_lightPreparation->PrepareResources(commandList, *m_RTXDIResources.LightIndices);

				commandList.End();
			}
		}
	}

	void PrepareReSTIRDI() {
		if (m_lightPreparation->GetEmissiveTriangleCount()) {
			auto& commandList = m_deviceResources->GetCommandList();

			commandList.Clear(*m_RTXDIResources.LocalLightPDF);

			{
				m_lightPreparation->GPUBuffers = {
					.InstanceData = m_GPUBuffers.InstanceData.get(),
					.ObjectData = m_GPUBuffers.ObjectData.get(),
					.LightInfo = m_RTXDIResources.LightInfo.get()
				};

				m_lightPreparation->Textures.LocalLightPDF = m_RTXDIResources.LocalLightPDF.get();

				m_lightPreparation->Process(commandList);
			}

			m_mipmapGeneration->SetTexture(*m_RTXDIResources.LocalLightPDF);

			m_mipmapGeneration->Process(commandList);
		}

		m_RTXDIResources.Context->SetLightBufferParams(m_lightPreparation->GetLightBufferParameters());

		const auto& settings = g_graphicsSettings.Raytracing.RTXDI.ReSTIRDI;

		{
			auto& context = m_RTXDIResources.Context->GetReGIRContext();

			auto dynamicParameters = context.GetReGIRDynamicParameters();
			dynamicParameters.regirCellSize = settings.ReGIR.Cell.Size;
			dynamicParameters.center = reinterpret_cast<const rtxdi::float3&>(m_cameraController.GetPosition());
			dynamicParameters.regirNumBuildSamples = settings.ReGIR.BuildSamples;
			context.SetDynamicParameters(dynamicParameters);
		}

		{
			auto& context = m_RTXDIResources.Context->GetReSTIRDIContext();

			context.SetFrameIndex(m_stepTimer.GetFrameCount() - 1);

			auto initialSamplingParameters = context.GetInitialSamplingParameters();
			initialSamplingParameters.numPrimaryLocalLightSamples = settings.InitialSampling.LocalLight.Samples;
			initialSamplingParameters.numPrimaryBrdfSamples = settings.InitialSampling.BRDFSamples;
			initialSamplingParameters.brdfCutoff = 0;
			initialSamplingParameters.localLightSamplingMode = settings.InitialSampling.LocalLight.Mode;
			context.SetInitialSamplingParameters(initialSamplingParameters);

			auto temporalResamplingParameters = context.GetTemporalResamplingParameters();
			temporalResamplingParameters.temporalBiasCorrection = settings.TemporalResampling.BiasCorrectionMode;
			temporalResamplingParameters.enableBoilingFilter = settings.TemporalResampling.BoilingFilter.IsEnabled;
			temporalResamplingParameters.boilingFilterStrength = settings.TemporalResampling.BoilingFilter.Strength;
			context.SetTemporalResamplingParameters(temporalResamplingParameters);

			auto spatialResamplingParameters = context.GetSpatialResamplingParameters();
			spatialResamplingParameters.spatialBiasCorrection = settings.SpatialResampling.BiasCorrectionMode;
			spatialResamplingParameters.numSpatialSamples = settings.SpatialResampling.Samples;
			context.SetSpatialResamplingParameters(spatialResamplingParameters);
		}
	}

	void RenderScene() {
		auto& commandList = m_deviceResources->GetCommandList();

		const auto FindTexture = [&](LPCWSTR name) {
			const auto pTexture = m_textures.find(name);
			return pTexture == cend(m_textures) ? nullptr : pTexture->second.get();
		};

		const auto
			position = FindTexture(TextureNames::Position),
			flatNormal = FindTexture(TextureNames::FlatNormal),
			geometricNormal = FindTexture(TextureNames::GeometricNormal),
			linearDepth = FindTexture(TextureNames::LinearDepth),
			motionVector = FindTexture(TextureNames::MotionVector),
			baseColorMetalness = FindTexture(TextureNames::BaseColorMetalness),
			normalRoughness = FindTexture(TextureNames::NormalRoughness),
			transmission = FindTexture(TextureNames::Transmission),
			IOR = FindTexture(TextureNames::IOR),
			lightRadiance = FindTexture(TextureNames::LightRadiance),
			radiance = FindTexture(TextureNames::Radiance),
			diffuse = FindTexture(TextureNames::Diffuse),
			specular = FindTexture(TextureNames::Specular),
			specularHitDistance = FindTexture(TextureNames::SpecularHitDistance);

		const auto& scene = m_scene->GetTopLevelAccelerationStructure();

		const auto& raytracingSettings = g_graphicsSettings.Raytracing;

		const auto& ReSTIRDISettings = raytracingSettings.RTXDI.ReSTIRDI;
		auto isReSTIRDIEnabled = ReSTIRDISettings.IsEnabled;
		if (isReSTIRDIEnabled) {
			PrepareReSTIRDI();
			isReSTIRDIEnabled &= m_RTXDIResources.LightInfo != nullptr;
		}

		const DenoisingSettings denoisingSettings{
			.Denoiser = ShouldDenoise() ? g_graphicsSettings.PostProcessing.Denoising.Denoiser : Denoiser::None,
			.NRDReBLURHitDistance = reinterpret_cast<const XMFLOAT4&>(nrd::ReblurSettings().hitDistanceParameters)
		};
		if (denoisingSettings.Denoiser != Denoiser::None) {
			commandList.Clear(*diffuse);
			commandList.Clear(*specular);
			if (denoisingSettings.Denoiser == ::Denoiser::DLSSRayReconstruction) {
				commandList.Clear(*specularHitDistance);
			}
		}

		{
			m_GBufferGeneration->GPUBuffers = {
				.SceneData = m_GPUBuffers.SceneData.get(),
				.Camera = m_GPUBuffers.Camera.get(),
				.InstanceData = m_GPUBuffers.InstanceData.get(),
				.ObjectData = m_GPUBuffers.ObjectData.get()
			};

			m_GBufferGeneration->Textures = {
				.Position = position,
				.FlatNormal = flatNormal,
				.GeometricNormal = geometricNormal,
				.LinearDepth = linearDepth,
				.NormalizedDepth = FindTexture(TextureNames::NormalizedDepth),
				.MotionVector = motionVector,
				.BaseColorMetalness = baseColorMetalness,
				.DiffuseAlbedo = diffuse,
				.SpecularAlbedo = specular,
				.NormalRoughness = normalRoughness,
				.Transmission = transmission,
				.IOR = IOR,
				.Radiance = radiance
			};

			m_GBufferGeneration->Render(
				commandList,
				scene,
				{
					.RenderSize = m_renderSize,
					.Flags = ~0u
						& ~(denoisingSettings.Denoiser == Denoiser::DLSSRayReconstruction ? 0 : GBufferGeneration::Flags::Albedo)
				}
			);
		}

		if (!m_scene->GetObjectCount()) {
			return;
		}

		if (isReSTIRDIEnabled) {
			m_RTXDI->GPUBuffers = {
				.Camera = m_GPUBuffers.Camera.get(),
				.ObjectData = m_GPUBuffers.ObjectData.get()
			};

			m_RTXDI->Textures = {
				.PreviousGeometricNormal = FindTexture(TextureNames::PreviousGeometricNormal),
				.GeometricNormal = geometricNormal,
				.PreviousLinearDepth = FindTexture(TextureNames::PreviousLinearDepth),
				.LinearDepth = linearDepth,
				.MotionVector = motionVector,
				.PreviousBaseColorMetalness = FindTexture(TextureNames::PreviousBaseColorMetalness),
				.BaseColorMetalness = baseColorMetalness,
				.PreviousNormalRoughness = FindTexture(TextureNames::PreviousNormalRoughness),
				.NormalRoughness = normalRoughness,
				.PreviousTransmission = FindTexture(TextureNames::PreviousTransmission),
				.Transmission = transmission,
				.PreviousIOR = FindTexture(TextureNames::PreviousIOR),
				.IOR = IOR,
				.Radiance = radiance,
				.LightRadiance = lightRadiance,
				.Diffuse = diffuse,
				.Specular = specular,
				.SpecularHitDistance = specularHitDistance
			};

			m_RTXDI->SetConstants(
				m_RTXDIResources,
				!raytracingSettings.Bounces,
				ReSTIRDISettings.ReGIR.Cell.IsVisualizationEnabled,
				!raytracingSettings.Bounces || raytracingSettings.RTXGI.Technique == RTXGITechnique::None ?
				denoisingSettings : DenoisingSettings()
			);

			m_RTXDI->Render(commandList, scene);
		}

		if (!raytracingSettings.Bounces) {
			return;
		}

		m_raytracing->GPUBuffers = {
			.SceneData = m_GPUBuffers.SceneData.get(),
			.Camera = m_GPUBuffers.Camera.get(),
			.ObjectData = m_GPUBuffers.ObjectData.get()
		};

		m_raytracing->Textures = {
			.Position = position,
			.FlatNormal = flatNormal,
			.GeometricNormal = geometricNormal,
			.LinearDepth = linearDepth,
			.BaseColorMetalness = baseColorMetalness,
			.NormalRoughness = normalRoughness,
			.Transmission = transmission,
			.IOR = IOR,
			.LightRadiance = lightRadiance,
			.Radiance = radiance,
			.Diffuse = diffuse,
			.Specular = specular,
			.SpecularHitDistance = specularHitDistance
		};

		m_raytracing->SetConstants({
			.RenderSize = m_renderSize,
			.FrameIndex = m_stepTimer.GetFrameCount() - 1,
			.Bounces = raytracingSettings.Bounces,
			.SamplesPerPixel = raytracingSettings.SamplesPerPixel,
			.IsRussianRouletteEnabled = raytracingSettings.IsRussianRouletteEnabled,
			.IsShaderExecutionReorderingEnabled = IsShaderExecutionReorderingEnabled(),
			.IsDIEnabled = isReSTIRDIEnabled,
			.Denoising = denoisingSettings
			});

		switch (raytracingSettings.RTXGI.Technique)
		{
			case RTXGITechnique::None: m_raytracing->Render(commandList, scene); break;

			case RTXGITechnique::SHARC:
			{
				Raytracing::SHARCSettings SHARCSettings{
					.DownscaleFactor = raytracingSettings.RTXGI.SHARC.DownscaleFactor,
					.RoughnessThreshold = raytracingSettings.RTXGI.SHARC.RoughnessThreshold,
					.IsHashGridVisualizationEnabled = raytracingSettings.RTXGI.SHARC.IsHashGridVisualizationEnabled
				};
				SHARCSettings.SceneScale = raytracingSettings.RTXGI.SHARC.SceneScale;
				SHARCSettings.IsAntiFireflyEnabled = true;
				m_raytracing->Render(commandList, scene, *m_SHARC, SHARCSettings);
			}
			break;
		}
	}

	static bool ShouldDenoise() {
		return g_graphicsSettings.Raytracing.Bounces || g_graphicsSettings.Raytracing.RTXDI.ReSTIRDI.IsEnabled;
	}

	bool IsDLSSFrameGenerationEnabled() const {
		return g_graphicsSettings.PostProcessing.IsDLSSFrameGenerationEnabled
			&& m_streamline->IsAvailable(sl::kFeatureDLSS_G) && IsReflexEnabled();
	}

	bool IsDLSSRayReconstructionEnabled() const {
		return g_graphicsSettings.PostProcessing.Denoising.Denoiser == Denoiser::DLSSRayReconstruction
			&& m_streamline->IsAvailable(sl::kFeatureDLSS_RR) && IsDLSSSuperResolutionEnabled();
	}

	bool IsDLSSSuperResolutionEnabled() const {
		return g_graphicsSettings.PostProcessing.SuperResolution.Upscaler == Upscaler::DLSS
			&& m_streamline->IsAvailable(sl::kFeatureDLSS);
	}

	bool IsNISEnabled() const {
		return g_graphicsSettings.PostProcessing.NIS.IsEnabled && m_streamline->IsAvailable(sl::kFeatureNIS);
	}

	bool IsNRDEnabled() const {
		const auto& denoiser = g_graphicsSettings.PostProcessing.Denoising.Denoiser;
		return (denoiser == Denoiser::NRDReBLUR || denoiser == Denoiser::NRDReLAX) && m_NRD->IsAvailable();
	}

	bool IsReflexEnabled() const {
		return g_graphicsSettings.ReflexMode != sl::ReflexMode::eOff && m_streamline->IsAvailable(sl::kFeatureReflex);
	}

	bool IsShaderExecutionReorderingEnabled() const {
		return m_isShaderExecutionReorderingAvailable && g_graphicsSettings.Raytracing.IsShaderExecutionReorderingEnabled;
	}

	bool IsXeSSSuperResolutionEnabled() const {
		return g_graphicsSettings.PostProcessing.SuperResolution.Upscaler == Upscaler::XeSS && m_XeSS->IsAvailable();
	}

	static void SetReflexOptions() {
		sl::ReflexOptions options;
		options.mode = g_graphicsSettings.ReflexMode;
		ignore = slReflexSetOptions(options);
	}

	auto CreateResourceTagDesc(sl::BufferType type, const Texture& texture, bool isRenderSize = true, sl::ResourceLifecycle lifecycle = sl::ResourceLifecycle::eValidUntilPresent) const {
		ResourceTagDesc resourceTagDesc{
			.Type = type,
			.Resource = sl::Resource(sl::ResourceType::eTex2d, texture, texture.GetState()),
			.Lifecycle = lifecycle
		};
		if (isRenderSize) {
			resourceTagDesc.Extent = { .width = m_renderSize.x, .height = m_renderSize.y };
		}
		return resourceTagDesc;
	}

	void SelectDenoiser() {
		const auto IsDLSSRayReconstructionAvailable = [&] {
			return m_streamline->IsAvailable(sl::kFeatureDLSS_RR) && IsDLSSSuperResolutionEnabled();
		};
		if (auto& denoiser = g_graphicsSettings.PostProcessing.Denoising.Denoiser;
			denoiser == Denoiser::DLSSRayReconstruction) {
			if (!IsDLSSRayReconstructionAvailable()) {
				denoiser = m_NRD->IsAvailable() ? Denoiser::NRDReLAX : Denoiser::None;
			}
		}
		else if (denoiser == Denoiser::NRDReBLUR || denoiser == Denoiser::NRDReLAX) {
			if (!m_NRD->IsAvailable()) {
				denoiser = IsDLSSRayReconstructionAvailable() ? Denoiser::DLSSRayReconstruction : Denoiser::None;
			}
		}
	}

	void SelectSuperResolutionUpscaler() {
		if (auto& upscaler = g_graphicsSettings.PostProcessing.SuperResolution.Upscaler;
			upscaler == Upscaler::DLSS) {
			if (!m_streamline->IsAvailable(sl::kFeatureDLSS)) {
				upscaler = m_XeSS->IsAvailable() ? Upscaler::XeSS : Upscaler::None;
			}
		}
		else if (upscaler == Upscaler::XeSS) {
			if (!m_XeSS->IsAvailable()) {
				upscaler = m_streamline->IsAvailable(sl::kFeatureDLSS) ? Upscaler::DLSS : Upscaler::None;
			}
		}
	}

	void SetSuperResolutionOptions() {
		const auto outputSize = GetOutputSize();

		const auto SelectSuperResolutionMode = [&] {
			if (g_graphicsSettings.PostProcessing.SuperResolution.Mode != SuperResolutionMode::Auto) {
				return g_graphicsSettings.PostProcessing.SuperResolution.Mode;
			}
			const auto pixelCount = outputSize.cx * outputSize.cy;
			if (pixelCount <= 1280 * 800) {
				return SuperResolutionMode::Native;
			}
			if (pixelCount <= 1920 * 1200) {
				return SuperResolutionMode::Quality;
			}
			if (pixelCount <= 2560 * 1600) {
				return SuperResolutionMode::Balanced;
			}
			if (pixelCount <= 3840 * 2400) {
				return SuperResolutionMode::Performance;
			}
			return SuperResolutionMode::UltraPerformance;
		};

		switch (g_graphicsSettings.PostProcessing.SuperResolution.Upscaler) {
			case Upscaler::None: m_renderSize = XMUINT2(outputSize.cx, outputSize.cy); break;

			case Upscaler::DLSS:
			{
				const auto Set = [&](auto& options) {
					switch (auto& mode = options.mode; SelectSuperResolutionMode()) {
						case SuperResolutionMode::Native: mode = sl::DLSSMode::eDLAA; break;
						case SuperResolutionMode::Quality: mode = sl::DLSSMode::eMaxQuality; break;
						case SuperResolutionMode::Balanced: mode = sl::DLSSMode::eBalanced; break;
						case SuperResolutionMode::Performance: mode = sl::DLSSMode::eMaxPerformance; break;
						case SuperResolutionMode::UltraPerformance: mode = sl::DLSSMode::eUltraPerformance; break;
						default: throw;
					}
					options.outputWidth = outputSize.cx;
					options.outputHeight = outputSize.cy;
					constexpr auto IsDLSS = is_same_v<sl::DLSSOptions, remove_cvref_t<decltype(options)>>;
					conditional_t<IsDLSS, sl::DLSSOptimalSettings, sl::DLSSDOptimalSettings> optimalSettings;
					if constexpr (IsDLSS) {
						ignore = slDLSSGetOptimalSettings(options, optimalSettings);
					}
					else {
						ignore = slDLSSDGetOptimalSettings(options, optimalSettings);
					}
					m_renderSize = XMUINT2(optimalSettings.optimalRenderWidth, optimalSettings.optimalRenderHeight);
					ignore = m_streamline->SetConstants(options);
				};
				if (IsDLSSRayReconstructionEnabled()) {
					Set(m_DLSSDOptions);
				}
				else {
					sl::DLSSOptions options;
					Set(options);
				}
			}
			break;

			case Upscaler::XeSS:
			{
				xess_quality_settings_t quality;
				switch (SelectSuperResolutionMode()) {
					case SuperResolutionMode::Native: quality = XESS_QUALITY_SETTING_AA; break;
					case SuperResolutionMode::Quality: quality = XESS_QUALITY_SETTING_QUALITY; break;
					case SuperResolutionMode::Balanced: quality = XESS_QUALITY_SETTING_BALANCED; break;
					case SuperResolutionMode::Performance: quality = XESS_QUALITY_SETTING_PERFORMANCE; break;
					case SuperResolutionMode::UltraPerformance: quality = XESS_QUALITY_SETTING_ULTRA_PERFORMANCE; break;
					default: throw;
				}
				ignore = m_XeSS->GetInputResolution(quality, reinterpret_cast<xess_2d_t&>(m_renderSize));
			}
			break;
		}

		OnRenderSizeChanged();
	}

	void SetDLSSFrameGenerationOptions(bool isEnabled) {
		sl::DLSSGOptions options;
		options.mode = isEnabled ? sl::DLSSGMode::eAuto : sl::DLSSGMode::eOff;
		ignore = m_streamline->SetConstants(options);
		m_isDLSSFrameGenerationEnabled = isEnabled;
	}

	void PostProcessGraphics() {
		const auto& postProcessingSettings = g_graphicsSettings.PostProcessing;

		auto inColor = m_textures.at(TextureNames::Radiance).get(), outColor = m_textures.at(TextureNames::Color).get();

		const auto
			shouldDenoise = ShouldDenoise(),
			isDLSSRayReconstructionEnabled = shouldDenoise && IsDLSSRayReconstructionEnabled(),
			isNRDEnabled = shouldDenoise && !isDLSSRayReconstructionEnabled && IsNRDEnabled(),
			isDLSSSuperResolutionEnabled = IsDLSSSuperResolutionEnabled(),
			isDLSSFrameGenerationEnabled = IsDLSSFrameGenerationEnabled();

		if (isDLSSSuperResolutionEnabled || isDLSSFrameGenerationEnabled) {
			PrepareStreamline();
		}

		if (isDLSSRayReconstructionEnabled) {
			ProcessDLSSRayReconstruction();

			swap(inColor, outColor);
		}
		else if (isNRDEnabled) {
			ProcessNRD();
		}

		if (!isDLSSRayReconstructionEnabled) {
			if (isDLSSSuperResolutionEnabled) {
				ProcessDLSSSuperResolution();

				swap(inColor, outColor);
			}
			else if (IsXeSSSuperResolutionEnabled()) {
				ProcessXeSSSuperResolution();

				swap(inColor, outColor);
			}
		}

		if (IsNISEnabled()) {
			ProcessNIS(*inColor, *outColor);

			swap(inColor, outColor);
		}

		if (postProcessingSettings.Bloom.IsEnabled) {
			ProcessBloom(*inColor, *outColor);

			swap(inColor, outColor);
		}

		{
			ToneMap(*inColor, *outColor);

			swap(inColor, outColor);
		}

		CopyTexture(*inColor);

		if (isDLSSFrameGenerationEnabled) {
			ProcessDLSSFrameGeneration(*inColor);
		}

		if (isNRDEnabled && postProcessingSettings.Denoising.IsNRDValidationOverlayEnabled) {
			AlphaBlendTexture(*m_textures.at(TextureNames::Validation));
		}
	}

	void PrepareStreamline() {
		sl::Constants constants;
		constants.cameraViewToClip = reinterpret_cast<const sl::float4x4&>(m_cameraController.GetViewToProjection());
		sl::recalculateCameraMatrices(constants, reinterpret_cast<const sl::float4x4&>(m_camera.PreviousViewToWorld), reinterpret_cast<const sl::float4x4&>(m_camera.PreviousViewToProjection));
		constants.jitterOffset = { -m_camera.Jitter.x, -m_camera.Jitter.y };
		constants.mvecScale = { 1 / static_cast<float>(m_renderSize.x), 1 / static_cast<float>(m_renderSize.y) };
		constants.cameraPinholeOffset = { 0, 0 };
		constants.cameraPos = reinterpret_cast<const sl::float3&>(m_cameraController.GetPosition());
		constants.cameraUp = reinterpret_cast<const sl::float3&>(m_cameraController.GetUpDirection());
		constants.cameraRight = reinterpret_cast<const sl::float3&>(m_cameraController.GetRightDirection());
		constants.cameraFwd = reinterpret_cast<const sl::float3&>(m_cameraController.GetForwardDirection());
		constants.cameraNear = m_cameraController.GetNearDepth();
		constants.cameraFar = m_cameraController.GetFarDepth();
		constants.cameraFOV = m_cameraController.GetVerticalFieldOfView();
		constants.cameraAspectRatio = m_cameraController.GetAspectRatio();
		constants.depthInverted = sl::Boolean::eTrue;
		constants.cameraMotionIncluded = sl::Boolean::eTrue;
		constants.motionVectors3D = sl::Boolean::eFalse;
		constants.reset = m_resetHistory ? sl::Boolean::eTrue : sl::Boolean::eFalse;
		ignore = m_streamline->SetConstants(constants);
	}

	void ProcessNRD() {
		auto& commandList = m_deviceResources->GetCommandList();

		const auto& denoisingSettings = g_graphicsSettings.PostProcessing.Denoising;

		auto
			& linearDepth = *m_textures.at(TextureNames::LinearDepth),
			& baseColorMetalness = *m_textures.at(TextureNames::BaseColorMetalness),
			& normalRoughness = *m_textures.at(TextureNames::NormalRoughness),
			& denoisedDiffuse = *m_textures.at(TextureNames::DenoisedDiffuse),
			& denoisedSpecular = *m_textures.at(TextureNames::DenoisedSpecular),
			& radiance = *m_textures.at(TextureNames::Radiance);

		{
			m_NRD->NewFrame();

			const auto Tag = [&](nrd::ResourceType resourceType, Texture& texture) { m_NRD->Tag(resourceType, texture); };
			Tag(nrd::ResourceType::IN_VIEWZ, linearDepth);
			Tag(nrd::ResourceType::IN_MV, *m_textures.at(TextureNames::MotionVector));
			Tag(nrd::ResourceType::IN_BASECOLOR_METALNESS, baseColorMetalness);
			Tag(nrd::ResourceType::IN_NORMAL_ROUGHNESS, normalRoughness);
			Tag(nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST, *m_textures.at(TextureNames::Diffuse));
			Tag(nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST, *m_textures.at(TextureNames::Specular));
			Tag(nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST, denoisedDiffuse);
			Tag(nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST, denoisedSpecular);
			Tag(nrd::ResourceType::OUT_VALIDATION, *m_textures.at(TextureNames::Validation));

			const auto outputSize = GetOutputSize();
			nrd::CommonSettings commonSettings{
				.motionVectorScale{ 1 / static_cast<float>(m_renderSize.x), 1 / static_cast<float>(m_renderSize.y), 1 },
				.resourceSize{ static_cast<uint16_t>(outputSize.cx), static_cast<uint16_t>(outputSize.cy) },
				.rectSize{ static_cast<uint16_t>(m_renderSize.x), static_cast<uint16_t>(m_renderSize.y) },
				.frameIndex = m_stepTimer.GetFrameCount() - 1,
				.accumulationMode = m_resetHistory ? nrd::AccumulationMode::CLEAR_AND_RESTART : nrd::AccumulationMode::CONTINUE,
				.isBaseColorMetalnessAvailable = true,
				.enableValidation = denoisingSettings.IsNRDValidationOverlayEnabled
			};
			reinterpret_cast<XMFLOAT4X4&>(commonSettings.viewToClipMatrix) = m_cameraController.GetViewToProjection();
			reinterpret_cast<XMFLOAT4X4&>(commonSettings.viewToClipMatrixPrev) = m_camera.PreviousViewToProjection;
			reinterpret_cast<XMFLOAT4X4&>(commonSettings.worldToViewMatrix) = m_cameraController.GetWorldToView();
			reinterpret_cast<XMFLOAT4X4&>(commonSettings.worldToViewMatrixPrev) = m_camera.PreviousWorldToView;
			reinterpret_cast<XMFLOAT2&>(commonSettings.cameraJitter) = m_camera.Jitter;
			ranges::copy(commonSettings.cameraJitter, commonSettings.cameraJitterPrev);
			ranges::copy(commonSettings.resourceSize, commonSettings.resourceSizePrev);
			ranges::copy(commonSettings.rectSize, commonSettings.rectSizePrev);
			ignore = m_NRD->SetConstants(commonSettings);

			const auto denoiser = static_cast<nrd::Identifier>(denoisingSettings.Denoiser);
			if (denoisingSettings.Denoiser == Denoiser::NRDReBLUR) {
				ignore = m_NRD->SetConstants(
					denoiser,
					nrd::ReblurSettings{
						.hitDistanceReconstructionMode = nrd::HitDistanceReconstructionMode::AREA_3X3,
						.enableAntiFirefly = true
					}
				);
			}
			else if (denoisingSettings.Denoiser == Denoiser::NRDReLAX) {
				ignore = m_NRD->SetConstants(
					denoiser,
					nrd::RelaxSettings{
						.hitDistanceReconstructionMode = nrd::HitDistanceReconstructionMode::AREA_3X3,
						.enableAntiFirefly = true
					}
				);
			}
			m_NRD->Denoise(initializer_list{ denoiser });
		}

		m_denoisedComposition->GPUBuffers = { .Camera = m_GPUBuffers.Camera.get() };

		m_denoisedComposition->Textures = {
			.LinearDepth = &linearDepth,
			.BaseColorMetalness = &baseColorMetalness,
			.NormalRoughness = &normalRoughness,
			.DenoisedDiffuse = &denoisedDiffuse,
			.DenoisedSpecular = &denoisedSpecular,
			.Radiance = &radiance
		};

		m_denoisedComposition->Process(commandList, { .RenderSize = m_renderSize, .Denoiser = denoisingSettings.Denoiser });
	}

	void ProcessDLSSSuperResolution() {
		ResourceTagDesc resourceTagDescs[]{
			CreateResourceTagDesc(sl::kBufferTypeDepth, *m_textures.at(TextureNames::NormalizedDepth)),
			CreateResourceTagDesc(sl::kBufferTypeMotionVectors, *m_textures.at(TextureNames::MotionVector)),
			CreateResourceTagDesc(sl::kBufferTypeScalingInputColor, *m_textures.at(TextureNames::Radiance)),
			CreateResourceTagDesc(sl::kBufferTypeScalingOutputColor, *m_textures.at(TextureNames::Color), false)
		};
		ignore = m_streamline->Evaluate(sl::kFeatureDLSS, CreateResourceTags(resourceTagDescs));
	}

	void ProcessDLSSRayReconstruction() {
		m_DLSSDOptions.normalRoughnessMode = sl::DLSSDNormalRoughnessMode::ePacked;
		m_DLSSDOptions.worldToCameraView = reinterpret_cast<const sl::float4x4&>(m_cameraController.GetWorldToView());
		m_DLSSDOptions.cameraViewToWorld = reinterpret_cast<const sl::float4x4&>(m_cameraController.GetViewToWorld());
		ignore = m_streamline->SetConstants(m_DLSSDOptions);

		ResourceTagDesc resourceTagDescs[]{
			CreateResourceTagDesc(sl::kBufferTypeDepth, *m_textures.at(TextureNames::NormalizedDepth)),
			CreateResourceTagDesc(sl::kBufferTypeMotionVectors, *m_textures.at(TextureNames::MotionVector)),
			CreateResourceTagDesc(sl::kBufferTypeScalingInputColor, *m_textures.at(TextureNames::Radiance)),
			CreateResourceTagDesc(sl::kBufferTypeScalingOutputColor, *m_textures.at(TextureNames::Color), false),
			CreateResourceTagDesc(sl::kBufferTypeNormalRoughness, *m_textures.at(TextureNames::NormalRoughness)),
			CreateResourceTagDesc(sl::kBufferTypeAlbedo, *m_textures.at(TextureNames::Diffuse)),
			CreateResourceTagDesc(sl::kBufferTypeSpecularAlbedo, *m_textures.at(TextureNames::Specular)),
			CreateResourceTagDesc(sl::kBufferTypeSpecularHitDistance, *m_textures.at(TextureNames::SpecularHitDistance))
		};
		ignore = m_streamline->Evaluate(sl::kFeatureDLSS_RR, CreateResourceTags(resourceTagDescs));
	}

	void ProcessDLSSFrameGeneration(Texture& inColor) {
		ResourceTagDesc resourceTagDescs[]{
			CreateResourceTagDesc(sl::kBufferTypeDepth, *m_textures.at(TextureNames::NormalizedDepth)),
			CreateResourceTagDesc(sl::kBufferTypeMotionVectors, *m_textures.at(TextureNames::MotionVector)),
			CreateResourceTagDesc(sl::kBufferTypeHUDLessColor, inColor, false)
		};
		ignore = m_streamline->Tag(CreateResourceTags(resourceTagDescs));
	}

	void ProcessXeSSSuperResolution() {
		auto& commandList = m_deviceResources->GetCommandList();

		const XeSSSettings settings{
			.InputSize = reinterpret_cast<const xess_2d_t&>(m_renderSize),
			.Jitter{ -m_camera.Jitter.x, -m_camera.Jitter.y },
			.Reset = m_resetHistory
		};
		m_XeSS->SetConstants(settings);

		struct XeSSResource {
			XeSSResourceType Type;
			Texture& Texture;
			D3D12_RESOURCE_STATES State = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
		};
		for (auto& [Type, Texture, State] : initializer_list<XeSSResource>{
			{ XeSSResourceType::Depth, *m_textures.at(TextureNames::NormalizedDepth) },
			{ XeSSResourceType::Velocity, *m_textures.at(TextureNames::MotionVector) },
			{ XeSSResourceType::Color, *m_textures.at(TextureNames::Radiance) },
			{ XeSSResourceType::Output, *m_textures.at(TextureNames::Color), D3D12_RESOURCE_STATE_UNORDERED_ACCESS }
			}) {
			commandList.SetState(Texture, State);
			m_XeSS->Tag(Type, Texture);
		}

		ignore = m_XeSS->Execute(commandList);
	}

	void ProcessNIS(Texture& inColor, Texture& outColor) {
		sl::NISOptions NISOptions;
		NISOptions.mode = sl::NISMode::eSharpen;
		NISOptions.sharpness = g_graphicsSettings.PostProcessing.NIS.Sharpness;
		ignore = m_streamline->SetConstants(NISOptions);

		ResourceTagDesc resourceTagDescs[]{
			CreateResourceTagDesc(sl::kBufferTypeScalingInputColor, inColor, false),
			CreateResourceTagDesc(sl::kBufferTypeScalingOutputColor, outColor, false)
		};
		ignore = m_streamline->Evaluate(sl::kFeatureNIS, CreateResourceTags(resourceTagDescs));
	}

	void ProcessBloom(Texture& inColor, Texture& outColor) {
		auto& commandList = m_deviceResources->GetCommandList();

		m_bloom->SetTextures(inColor, outColor);

		m_bloom->Process(commandList, { .Strength = g_graphicsSettings.PostProcessing.Bloom.Strength });
	}

	void ToneMap(Texture& inColor, Texture& outColor) {
		auto& commandList = m_deviceResources->GetCommandList();

		commandList.SetState(inColor, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		commandList.SetState(outColor, D3D12_RESOURCE_STATE_RENDER_TARGET);

		const auto isHDREnabled = m_deviceResources->IsHDREnabled();
		const auto toneMappingSettings = g_graphicsSettings.PostProcessing.ToneMapping;

		auto& toneMapping = *m_toneMapping[(isHDREnabled ? ToneMapPostProcess::Operator_Max : toneMappingSettings.NonHDR.Operator) - 1];

		if (isHDREnabled) {
			toneMapping.SetST2084Parameter(toneMappingSettings.HDR.PaperWhiteNits);
			toneMapping.SetColorRotation(toneMappingSettings.HDR.ColorPrimaryRotation);
		}
		else {
			toneMapping.SetExposure(toneMappingSettings.NonHDR.Exposure);
		}

		toneMapping.SetHDRSourceTexture(inColor.GetSRVDescriptor());

		commandList.SetRenderTarget(outColor);

		toneMapping.Process(commandList);

		commandList.SetRenderTarget(m_deviceResources->GetBackBuffer());
	}

	void CopyTexture(Texture& inColor) {
		auto& commandList = m_deviceResources->GetCommandList();

		commandList.SetState(inColor, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		m_textureCopy->SetSourceTexture(inColor.GetSRVDescriptor(), inColor);
		m_textureCopy->Process(commandList);
	}

	void AlphaBlendTexture(Texture& inColor) {
		auto& commandList = m_deviceResources->GetCommandList();

		commandList.SetState(inColor, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		m_textureAlphaBlending->SetViewport(m_deviceResources->GetScreenViewport());
		m_textureAlphaBlending->Begin(commandList);
		m_textureAlphaBlending->Draw(inColor.GetSRVDescriptor(), GetTextureSize(inColor), XMFLOAT2());
		m_textureAlphaBlending->End();
	}

	void RenderUI() {
		ImGui_ImplDX12_NewFrame();
		ImGui_ImplWin32_NewFrame();

		const auto outputSize = GetOutputSize();
		ImGui::GetIO().DisplaySize = { static_cast<float>(outputSize.cx), static_cast<float>(outputSize.cy) };

		ImGui::NewFrame();

		const auto popupModalName = RenderMainMenuBar();

		if (m_UIStates.IsSettingsWindowOpen) {
			RenderSettingsWindow();
		}

		RenderPopupModalWindow(popupModalName);

		if (IsSceneLoading()) {
			RenderLoadingSceneWindow();
		}

		ImGui::Render();
		ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), m_deviceResources->GetCommandList());
	}

	string RenderMainMenuBar() {
		string popupModalName;
		if (ImGuiEx::MainMenuBar mainMenuBar; mainMenuBar) {
			if (ImGui::GetFrameCount() == 1) {
				ImGui::SetKeyboardFocusHere();
			}

			const auto PopupModal = [&](const char* name) {
				if (ImGui::MenuItem(name)) {
					popupModalName = name;
				}
			};

			if (ImGuiEx::Menu menu("File"); menu) {
				if (ImGui::MenuItem("Exit")) {
					PostQuitMessage(ERROR_SUCCESS);
				}
			}

			if (ImGuiEx::Menu menu("View"); menu) {
				m_UIStates.IsSettingsWindowOpen |= ImGui::MenuItem("Settings");
			}

			if (ImGuiEx::Menu menu("Help"); menu) {
				PopupModal("Controls");

				ImGui::Separator();

				PopupModal("About");
			}
		}
		return popupModalName;
	}

	void RenderSettingsWindow() {
		ImGui::SetNextWindowBgAlpha(g_UISettings.WindowOpacity);

		const auto& viewport = *ImGui::GetMainViewport();
		ImGui::SetNextWindowPos({ viewport.WorkPos.x, viewport.WorkPos.y });
		ImGui::SetNextWindowSize({});

		if (ImGuiEx::Window window("Settings", &m_UIStates.IsSettingsWindowOpen, ImGuiWindowFlags_HorizontalScrollbar); window) {
			if (ImGuiEx::TreeNode treeNode("Graphics"); treeNode) {
				{
					auto isChanged = false;

					if (ImGuiEx::Combo<WindowMode>(
						"Window Mode",
						{ WindowMode::Windowed, WindowMode::Borderless, WindowMode::Fullscreen },
						g_graphicsSettings.WindowMode,
						g_graphicsSettings.WindowMode,
						static_cast<string(*)(WindowMode)>(ToString)
						)) {
						m_windowModeHelper.SetMode(g_graphicsSettings.WindowMode);

						isChanged = true;
					}

					if (ImGuiEx::Combo<Resolution, set>(
						"Resolution",
						g_displayResolutions,
						g_graphicsSettings.Resolution,
						g_graphicsSettings.Resolution,
						[](Resolution value) { return format("{} × {}", value.cx, value.cy); }
						)) {
						m_windowModeHelper.SetResolution(g_graphicsSettings.Resolution);

						isChanged = true;
					}

					if (isChanged) {
						m_futures["WindowSetting"] = async(
							launch::deferred,
							[&] { ThrowIfFailed(m_windowModeHelper.Apply()); }
						);
					}
				}

				{
					auto isEnabled = m_deviceResources->IsHDREnabled();
					if (const ImGuiEx::Enablement enablement(m_deviceResources->IsHDRSupported());
						ImGui::Checkbox("HDR", &isEnabled)) {
						g_graphicsSettings.IsHDREnabled = isEnabled;

						m_futures["HDRSetting"] = async(
							launch::deferred,
							[&] { m_deviceResources->RequestHDR(g_graphicsSettings.IsHDREnabled); }
						);
					}
				}

				{
					auto isEnabled = m_deviceResources->IsVSyncEnabled();
					if (const ImGuiEx::Enablement enablement(m_deviceResources->IsTearingSupported());
						ImGui::Checkbox("V-Sync", &isEnabled)) {
						g_graphicsSettings.IsVSyncEnabled = isEnabled;

						m_deviceResources->EnableVSync(isEnabled);
					}
				}

				if (const ImGuiEx::Enablement enablement(m_isReflexLowLatencyAvailable);
					ImGuiEx::Combo<sl::ReflexMode>(
						"NVIDIA Reflex",
						{ sl::ReflexMode::eOff, sl::ReflexMode::eLowLatency, sl::ReflexMode::eLowLatencyWithBoost },
						m_isReflexLowLatencyAvailable ? g_graphicsSettings.ReflexMode : sl::ReflexMode::eOff,
						g_graphicsSettings.ReflexMode,
						static_cast<string(*)(sl::ReflexMode)>(ToString)
						)) {
					m_futures["ReflexSetting"] = async(
						launch::deferred,
						[&] { SetReflexOptions(); }
					);
				}

				if (ImGuiEx::TreeNode treeNode("Camera", ImGuiTreeNodeFlags_DefaultOpen); treeNode) {
					auto& cameraSettings = g_graphicsSettings.Camera;

					m_resetHistory |= ImGui::Checkbox("Jitter", &cameraSettings.IsJitterEnabled);

					if (ImGui::SliderFloat("Horizontal Field of View", &cameraSettings.HorizontalFieldOfView, cameraSettings.MinHorizontalFieldOfView, cameraSettings.MaxHorizontalFieldOfView, "%.1f°", ImGuiSliderFlags_AlwaysClamp)) {
						m_cameraController.SetLens(XMConvertToRadians(cameraSettings.HorizontalFieldOfView), m_cameraController.GetAspectRatio());
					}
				}

				if (ImGuiEx::TreeNode treeNode("Raytracing", ImGuiTreeNodeFlags_DefaultOpen); treeNode) {
					auto& raytracingSettings = g_graphicsSettings.Raytracing;

					m_resetHistory |= ImGui::Checkbox("Russian Roulette", &raytracingSettings.IsRussianRouletteEnabled);

					m_resetHistory |= ImGui::SliderInt("Bounces", reinterpret_cast<int*>(&raytracingSettings.Bounces), 0, raytracingSettings.MaxBounces, "%u", ImGuiSliderFlags_AlwaysClamp);

					if (raytracingSettings.Bounces) {
						m_resetHistory |= ImGui::SliderInt("Samples/Pixel", reinterpret_cast<int*>(&raytracingSettings.SamplesPerPixel), 1, raytracingSettings.MaxSamplesPerPixel, "%u", ImGuiSliderFlags_AlwaysClamp);
					}

					{
						auto isEnabled = IsShaderExecutionReorderingEnabled();
						if (const ImGuiEx::Enablement enablement(m_isShaderExecutionReorderingAvailable);
							ImGui::Checkbox("NVIDIA Shader Execution Reordering", &isEnabled)) {
							raytracingSettings.IsShaderExecutionReorderingEnabled = isEnabled;
						}
					}

					if (ImGuiEx::TreeNode treeNode("NVIDIA RTXDI", ImGuiTreeNodeFlags_DefaultOpen); treeNode) {
						auto& RTXDISettings = raytracingSettings.RTXDI;

						if (ImGuiEx::TreeNode treeNode("ReSTIR DI", ImGuiTreeNodeFlags_DefaultOpen); treeNode) {
							auto& ReSTIRDISettings = RTXDISettings.ReSTIRDI;

							m_resetHistory |= ImGui::Checkbox("Enable", &ReSTIRDISettings.IsEnabled);

							if (ReSTIRDISettings.IsEnabled) {
								if (ReSTIRDISettings.InitialSampling.LocalLight.Mode == ReSTIRDI_LocalLightSamplingMode::ReGIR_RIS) {
									if (ImGuiEx::TreeNode treeNode("ReGIR", ImGuiTreeNodeFlags_DefaultOpen); treeNode) {
										auto& ReGIRSettings = ReSTIRDISettings.ReGIR;

										if (ImGuiEx::TreeNode treeNode("Cell", ImGuiTreeNodeFlags_DefaultOpen); treeNode) {
											auto& cellSettings = ReGIRSettings.Cell;

											m_resetHistory |= ImGui::SliderFloat("Size", &cellSettings.Size, cellSettings.MinSize, cellSettings.MaxSize, "%.2f", ImGuiSliderFlags_AlwaysClamp);

											m_resetHistory |= ImGui::Checkbox("Visualization", &cellSettings.IsVisualizationEnabled);
										}

										m_resetHistory |= ImGui::SliderInt("Build Samples", reinterpret_cast<int*>(&ReGIRSettings.BuildSamples), 1, ReGIRSettings.MaxBuildSamples, "%u", ImGuiSliderFlags_AlwaysClamp);
									}
								}

								if (ImGuiEx::TreeNode treeNode("Initial Sampling", ImGuiTreeNodeFlags_DefaultOpen); treeNode) {
									auto& initialSamplingSettings = ReSTIRDISettings.InitialSampling;

									if (ImGuiEx::TreeNode treeNode("Local Light", ImGuiTreeNodeFlags_DefaultOpen); treeNode) {
										auto& localLightSettings = initialSamplingSettings.LocalLight;

										m_resetHistory |= ImGuiEx::Combo<ReSTIRDI_LocalLightSamplingMode>(
											"Mode",
											{
												ReSTIRDI_LocalLightSamplingMode::Uniform,
												ReSTIRDI_LocalLightSamplingMode::Power_RIS,
												ReSTIRDI_LocalLightSamplingMode::ReGIR_RIS
											},
											localLightSettings.Mode,
											localLightSettings.Mode,
											static_cast<string(*)(ReSTIRDI_LocalLightSamplingMode)>(ToString)
											);

										m_resetHistory |= ImGui::SliderInt("Samples", reinterpret_cast<int*>(&localLightSettings.Samples), 1, localLightSettings.MaxSamples, "%u", ImGuiSliderFlags_AlwaysClamp);
									}

									m_resetHistory |= ImGui::SliderInt("BRDF Samples", reinterpret_cast<int*>(&initialSamplingSettings.BRDFSamples), 1, initialSamplingSettings.MaxBRDFSamples, "%u", ImGuiSliderFlags_AlwaysClamp);
								}

								if (ImGuiEx::TreeNode treeNode("Temporal Resampling", ImGuiTreeNodeFlags_DefaultOpen); treeNode) {
									auto& temporalResamplingSettings = ReSTIRDISettings.TemporalResampling;

									m_resetHistory |= ImGuiEx::Combo<ReSTIRDI_TemporalBiasCorrectionMode>(
										"Bias Correction Mode",
										{
											ReSTIRDI_TemporalBiasCorrectionMode::Off,
											ReSTIRDI_TemporalBiasCorrectionMode::Basic,
											ReSTIRDI_TemporalBiasCorrectionMode::Pairwise,
											ReSTIRDI_TemporalBiasCorrectionMode::Raytraced
										},
										temporalResamplingSettings.BiasCorrectionMode,
										temporalResamplingSettings.BiasCorrectionMode,
										static_cast<string(*)(ReSTIRDI_TemporalBiasCorrectionMode)>(ToString)
										);

									if (ImGuiEx::TreeNode treeNode("Boiling Filter", ImGuiTreeNodeFlags_DefaultOpen); treeNode) {
										auto& boilingFilterSettings = temporalResamplingSettings.BoilingFilter;

										m_resetHistory |= ImGui::Checkbox("Enable", &boilingFilterSettings.IsEnabled);

										m_resetHistory |= ImGui::SliderFloat("Strength", &boilingFilterSettings.Strength, 0, 1, "%.2f", ImGuiSliderFlags_AlwaysClamp);
									}
								}

								if (ImGuiEx::TreeNode treeNode("Spatial Resampling", ImGuiTreeNodeFlags_DefaultOpen); treeNode) {
									auto& spatialResamplingSettings = ReSTIRDISettings.SpatialResampling;

									m_resetHistory |= ImGuiEx::Combo<ReSTIRDI_SpatialBiasCorrectionMode>(
										"Bias Correction Mode",
										{
											ReSTIRDI_SpatialBiasCorrectionMode::Off,
											ReSTIRDI_SpatialBiasCorrectionMode::Basic,
											ReSTIRDI_SpatialBiasCorrectionMode::Pairwise,
											ReSTIRDI_SpatialBiasCorrectionMode::Raytraced
										},
										spatialResamplingSettings.BiasCorrectionMode,
										spatialResamplingSettings.BiasCorrectionMode,
										static_cast<string(*)(ReSTIRDI_SpatialBiasCorrectionMode)>(ToString)
										);

									m_resetHistory |= ImGui::SliderInt("Samples", reinterpret_cast<int*>(&spatialResamplingSettings.Samples), 1, spatialResamplingSettings.MaxSamples, "%u", ImGuiSliderFlags_AlwaysClamp);
								}
							}
						}
					}

					if (raytracingSettings.Bounces) {
						if (ImGuiEx::TreeNode treeNode("NVIDIA RTXGI", ImGuiTreeNodeFlags_DefaultOpen); treeNode) {
							auto& RTXGISettings = raytracingSettings.RTXGI;

							m_resetHistory |= ImGuiEx::Combo<RTXGITechnique>(
								"Technique",
								{ RTXGITechnique::None, RTXGITechnique::SHARC },
								RTXGISettings.Technique,
								RTXGISettings.Technique,
								static_cast<string(*)(RTXGITechnique)>(ToString)
								);

							if (RTXGISettings.Technique == RTXGITechnique::SHARC) {
								auto& SHARCSettings = RTXGISettings.SHARC;

								m_resetHistory |= ImGui::SliderInt("Downscale Factor", reinterpret_cast<int*>(&SHARCSettings.DownscaleFactor), 1, SHARCSettings.MaxDownscaleFactor, "%u", ImGuiSliderFlags_AlwaysClamp);

								m_resetHistory |= ImGui::SliderFloat("Scene Scale", &SHARCSettings.SceneScale, SHARCSettings.MinSceneScale, SHARCSettings.MaxSceneScale, "%.2f", ImGuiSliderFlags_AlwaysClamp);

								m_resetHistory |= ImGui::SliderFloat("Roughness Threshold", &SHARCSettings.RoughnessThreshold, 0, 1, "%.2f", ImGuiSliderFlags_AlwaysClamp);

								m_resetHistory |= ImGui::Checkbox("Hash Grid Visualization", &SHARCSettings.IsHashGridVisualizationEnabled);
							}
						}
					}
				}

				if (ImGuiEx::TreeNode treeNode("Post-Processing", ImGuiTreeNodeFlags_DefaultOpen); treeNode) {
					auto& postProcessingSetttings = g_graphicsSettings.PostProcessing;

					if (ImGuiEx::TreeNode treeNode("Super Resolution", ImGuiTreeNodeFlags_DefaultOpen); treeNode) {
						auto& superResolutionSettings = postProcessingSetttings.SuperResolution;

						auto isChanged = false;

						{
							const auto IsSelectable = [&](Upscaler upscaler) {
								return upscaler == Upscaler::None
									|| (upscaler == Upscaler::DLSS && m_streamline->IsAvailable(sl::kFeatureDLSS))
									|| (upscaler == Upscaler::XeSS && m_XeSS->IsAvailable()) ?
									ImGuiSelectableFlags_None : ImGuiSelectableFlags_Disabled;
							};
							if (ImGuiEx::Combo<Upscaler>(
								"Upscaler",
								{ Upscaler::None, Upscaler::DLSS, Upscaler::XeSS },
								superResolutionSettings.Upscaler,
								superResolutionSettings.Upscaler,
								static_cast<string(*)(Upscaler)>(ToString),
								IsSelectable
								)) {
								if (superResolutionSettings.Upscaler != Upscaler::DLSS) {
									SelectDenoiser();
								}

								isChanged = true;
							}
						}

						if (superResolutionSettings.Upscaler != Upscaler::None) {
							isChanged |= ImGuiEx::Combo<SuperResolutionMode>(
								"Mode",
								{
									SuperResolutionMode::Auto,
									SuperResolutionMode::Native,
									SuperResolutionMode::Quality,
									SuperResolutionMode::Balanced,
									SuperResolutionMode::Performance,
									SuperResolutionMode::UltraPerformance
								},
								superResolutionSettings.Mode,
								superResolutionSettings.Mode,
								static_cast<string(*)(SuperResolutionMode)>(ToString)
								);
						}

						if (isChanged) {
							m_futures["SuperResolutionSetting"] = async(
								launch::deferred,
								[&] { SetSuperResolutionOptions(); }
							);
						}
					}

					if (ShouldDenoise()) {
						if (ImGuiEx::TreeNode treeNode("Denoising", ImGuiTreeNodeFlags_DefaultOpen); treeNode) {
							auto& denoisingSettings = postProcessingSetttings.Denoising;

							{
								const auto IsSelectable = [&](Denoiser denoiser) {
									return denoiser == Denoiser::None
										|| (denoiser == Denoiser::DLSSRayReconstruction
											&& m_streamline->IsAvailable(sl::kFeatureDLSS_RR))
										|| ((denoiser == Denoiser::NRDReBLUR || denoiser == Denoiser::NRDReLAX)
											&& m_NRD->IsAvailable()) ?
										ImGuiSelectableFlags_None : ImGuiSelectableFlags_Disabled;
								};
								const auto
									denoisers = {
									Denoiser::None, Denoiser::DLSSRayReconstruction, Denoiser::NRDReBLUR, Denoiser::NRDReLAX
								},
									denoisers1 = {
									Denoiser::None, Denoiser::NRDReBLUR, Denoiser::NRDReLAX
								};
								const auto denoiser = denoisingSettings.Denoiser;
								if (ImGuiEx::Combo<Denoiser>(
									"Denoiser",
									IsDLSSSuperResolutionEnabled() ? denoisers : denoisers1,
									denoisingSettings.Denoiser,
									denoisingSettings.Denoiser,
									static_cast<string(*)(Denoiser)>(ToString),
									IsSelectable
									)) {
									if (denoiser == Denoiser::DLSSRayReconstruction
										|| denoisingSettings.Denoiser == Denoiser::DLSSRayReconstruction) {
										m_futures["DLSSRayReconstructionSetting"] = async(
											launch::deferred,
											[&] { SetSuperResolutionOptions(); }
										);
									}

									m_resetHistory = true;
								}
							}

							if (denoisingSettings.Denoiser == Denoiser::NRDReBLUR
								|| denoisingSettings.Denoiser == Denoiser::NRDReLAX) {
								ImGui::Checkbox("Validation Overlay", &denoisingSettings.IsNRDValidationOverlayEnabled);
							}
						}
					}

					if (IsReflexEnabled()) {
						auto isEnabled = IsDLSSFrameGenerationEnabled();
						if (const ImGuiEx::Enablement enablement(m_streamline->IsAvailable(sl::kFeatureDLSS_G));
							ImGui::Checkbox("NVIDIA DLSS Frame Generation", &isEnabled)) {
							postProcessingSetttings.IsDLSSFrameGenerationEnabled = isEnabled;
						}
					}

					{
						const auto isAvailable = m_streamline->IsAvailable(sl::kFeatureNIS);
						const ImGuiEx::Enablement enablement(isAvailable);
						if (ImGuiEx::TreeNode treeNode("NVIDIA Image Scaling", isAvailable ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None);
							treeNode) {
							auto& NISSettings = postProcessingSetttings.NIS;

							ImGui::Checkbox("Enable", &NISSettings.IsEnabled);

							if (NISSettings.IsEnabled) {
								ImGui::SliderFloat("Sharpness", &NISSettings.Sharpness, 0, 1, "%.2f", ImGuiSliderFlags_AlwaysClamp);
							}
						}
					}

					if (ImGuiEx::TreeNode treeNode("Bloom", ImGuiTreeNodeFlags_DefaultOpen); treeNode) {
						auto& bloomSettings = postProcessingSetttings.Bloom;

						ImGui::Checkbox("Enable", &bloomSettings.IsEnabled);

						if (bloomSettings.IsEnabled) {
							ImGui::SliderFloat("Strength", &bloomSettings.Strength, 0, 1, "%.2f", ImGuiSliderFlags_AlwaysClamp);
						}
					}

					if (ImGuiEx::TreeNode treeNode("Tone Mapping", ImGuiTreeNodeFlags_DefaultOpen); treeNode) {
						auto& toneMappingSettings = postProcessingSetttings.ToneMapping;

						if (m_deviceResources->IsHDREnabled()) {
							auto& HDRSettings = toneMappingSettings.HDR;

							ImGui::SliderFloat("Paper White Nits", &HDRSettings.PaperWhiteNits, HDRSettings.MinPaperWhiteNits, HDRSettings.MaxPaperWhiteNits, "%.1f", ImGuiSliderFlags_AlwaysClamp);

							ImGuiEx::Combo<ToneMapPostProcess::ColorPrimaryRotation>(
								"Color Primary Rotation",
								{
									ToneMapPostProcess::HDTV_to_UHDTV,
									ToneMapPostProcess::DCI_P3_D65_to_UHDTV,
									ToneMapPostProcess::HDTV_to_DCI_P3_D65
								},
								HDRSettings.ColorPrimaryRotation,
								HDRSettings.ColorPrimaryRotation,
								static_cast<string(*)(ToneMapPostProcess::ColorPrimaryRotation)>(ToString)
								);
						}
						else {
							auto& nonHDRSettings = toneMappingSettings.NonHDR;

							ImGuiEx::Combo<ToneMapPostProcess::Operator>(
								"Operator",
								{
									ToneMapPostProcess::Saturate,
									ToneMapPostProcess::Reinhard,
									ToneMapPostProcess::ACESFilmic
								},
								nonHDRSettings.Operator,
								nonHDRSettings.Operator,
								static_cast<string(*)(ToneMapPostProcess::Operator)>(ToString)
								);

							ImGui::SliderFloat("Exposure", &nonHDRSettings.Exposure, nonHDRSettings.MinExposure, nonHDRSettings.MaxExposure, "%.2f", ImGuiSliderFlags_AlwaysClamp);
						}
					}
				}
			}

			if (ImGuiEx::TreeNode treeNode("UI"); treeNode) {
				ImGui::Checkbox("Show on Startup", &g_UISettings.ShowOnStartup);

				ImGui::SliderFloat("Window Opacity", &g_UISettings.WindowOpacity, 0, 1, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			}

			if (ImGuiEx::TreeNode treeNode("Controls"); treeNode) {
				if (ImGuiEx::TreeNode treeNode("Camera", ImGuiTreeNodeFlags_DefaultOpen); treeNode) {
					auto& cameraSettings = g_controlsSettings.Camera;

					if (ImGuiEx::TreeNode treeNode("Speed", ImGuiTreeNodeFlags_DefaultOpen); treeNode) {
						auto& speedSettings = cameraSettings.Speed;

						ImGui::SliderFloat("Movement", &speedSettings.Movement, 0.0f, speedSettings.MaxMovement, "%.1f", ImGuiSliderFlags_AlwaysClamp);

						ImGui::SliderFloat("Rotation", &speedSettings.Rotation, 0.0f, speedSettings.MaxRotation, "%.2f", ImGuiSliderFlags_AlwaysClamp);
					}
				}
			}

			if (ImGui::Button("Save")) {
				ignore = MyAppData::Settings::Save();
			}
		}
	}

	void RenderPopupModalWindow(string_view popupModalName) {
		const auto PopupModal = [&](const char* name, const auto& lambda) {
			if (name == popupModalName) {
				ImGui::OpenPopup(name);
			}

			ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetWorkCenter(), ImGuiCond_Always, { 0.5f, 0.5f });
			ImGui::SetNextWindowSize({});

			if (ImGuiEx::PopupModal popupModal(name, nullptr, ImGuiWindowFlags_HorizontalScrollbar); popupModal) {
				lambda();

				ImGui::Separator();

				{
					constexpr auto Text = "OK";
					ImGuiEx::AlignForWidth(ImGui::CalcTextSize(Text).x);
					if (ImGui::Button(Text)) {
						ImGui::CloseCurrentPopup();
					}
					ImGui::SetItemDefaultFocus();
				}
			}
		};

		PopupModal(
			"Controls",
			[] {
				const auto AddWidgets = [](const char* treeLabel, const char* tableID, initializer_list<pair<const char*, const char*>> list) {
				if (ImGuiEx::TreeNode treeNode(treeLabel, ImGuiTreeNodeFlags_DefaultOpen); treeNode) {
					if (ImGuiEx::Table table(tableID, 2, ImGuiTableFlags_Borders); table) {
						for (const auto& [first, second] : list) {
							ImGui::TableNextRow();

							ImGui::TableSetColumnIndex(0);
							ImGui::Text(first);

							ImGui::TableSetColumnIndex(1);
							ImGui::Text(second);
						}
					}
				}
			};

		AddWidgets(
			"Xbox Controller",
			"##XboxController",
			{
				{ "Menu", "Show/hide UI" },
				{ "X (hold)", "Show window switcher when UI visible" },
				{ "View", "Reset camera" },
				{ "LS (rotate)", "Move" },
				{ "RS (rotate)", "Look around" },
				{ "D-Pad Up Down", "Change camera movement speed" },
				{ "A", "Run/pause physics simulation" },
				{ "B", "Toggle gravity of Earth" },
				{ "Y", "Toggle gravity of the star" }
			}
		);

		AddWidgets(
			"Keyboard",
			"##Keyboard",
			{
				{ "Alt + Enter", "Toggle between windowed/borderless & fullscreen modes" },
				{ "Esc", "Show/hide UI" },
				{ "Ctrl + Tab (hold)", "Show window switcher when UI visible" },
				{ "Home", "Reset camera" },
				{ "W A S D", "Move" },
				{ "Space", "Run/pause physics simulation" },
				{ "G", "Toggle gravity of Earth" },
				{ "H", "Toggle gravity of the star" }
			}
		);

		AddWidgets(
			"Mouse",
			"##Mouse",
			{
				{ "(Move)", "Look around" },
				{ "Scroll Wheel", "Change camera movement speed" }
			}
		);
			}
		);

		PopupModal(
			"About",
			[] {
				ImGui::Text("© Hydr10n. All rights reserved.");

		if (constexpr auto URL = "https://github.com/Hydr10n/DirectX-Raytracing-Spheres-Demo";
			ImGuiEx::Hyperlink("GitHub repository", URL)) {
			ShellExecuteA(nullptr, "open", URL, nullptr, nullptr, SW_SHOW);
		}
			}
		);
	}

	void RenderLoadingSceneWindow() {
		ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetWorkCenter(), ImGuiCond_Always, { 0.5f, 0.5f });
		ImGui::SetNextWindowSize({});

		const auto label = "Loading Scene";
		if (ImGuiEx::Window window(label, nullptr, ImGuiWindowFlags_NoTitleBar); window) {
			{
				const auto radius = static_cast<float>(GetOutputSize().cy) * 0.01f;
				ImGuiEx::Spinner(label, ImGui::GetColorU32(ImGuiCol_Button), radius, radius * 0.4f);
			}

			ImGui::SameLine();

			ImGui::Text(label);
		}
	}
};

App::App(WindowModeHelper& windowModeHelper) : m_impl(make_unique<Impl>(windowModeHelper)) {}

App::~App() = default;

SIZE App::GetOutputSize() const noexcept { return m_impl->GetOutputSize(); }

void App::Tick() { m_impl->Tick(); }

void App::OnWindowSizeChanged() { m_impl->OnWindowSizeChanged(); }

void App::OnDisplayChanged() { m_impl->OnDisplayChanged(); }

void App::OnResuming() { m_impl->OnResuming(); }

void App::OnSuspending() { m_impl->OnSuspending(); }

void App::OnActivated() { m_impl->OnActivated(); }

void App::OnDeactivated() { m_impl->OnDeactivated(); }
