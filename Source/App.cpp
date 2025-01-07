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

#include "rtxdi/ImportanceSamplingContext.h"

#include "sl_helpers.h"
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
import GPUBuffer;
import HaltonSamplePattern;
import LightPreparation;
import Material;
import MyScene;
import PostProcessing.Bloom;
import PostProcessing.ChromaticAberration;
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
using namespace nrd;
using namespace PostProcessing;
using namespace rtxdi;
using namespace SharedData;
using namespace sl;
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
			if (ImGui::GetIO().BackendRendererUserData != nullptr) ImGui_ImplDX12_Shutdown();

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
				{
					const auto ret = future.second.wait_for(0s) == future_status::deferred;
					if (ret) future.second.get();
					return ret;
				}
			}
		);

		m_inputDevices.Mouse->EndOfInputFrame();

		{
			const scoped_lock lock(m_exceptionMutex);

			if (m_exception) rethrow_exception(m_exception);
		}
	}

	void OnWindowSizeChanged() {
		if (m_deviceResources->ResizeWindow(m_windowModeHelper.GetResolution())) CreateWindowSizeDependentResources();
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
		if (ImGui::GetIO().BackendRendererUserData != nullptr) ImGui_ImplDX12_Shutdown();

		m_scene.reset();

		m_GPUBuffers = {};

		m_textures = {};

		m_mipmapGeneration.reset();

		m_textureAlphaBlending.reset();

		m_textureCopy.reset();

		for (auto& toneMapping : m_toneMapping) toneMapping.reset();

		m_bloom.reset();

		m_chromaticAberration.reset();

		m_XeSS.reset();

		m_denoisedComposition.reset();
		m_NRD.reset();

		m_streamline.reset();

		m_RTXDI.reset();
		m_lightPreparation.reset();
		m_RTXDIResources = {};

		m_raytracing.reset();

		m_SHARC.reset();

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
		.OptionFlags = DeviceResources::OptionFlags::ReverseDepth | DeviceResources::OptionFlags::DisableGPUTimeout
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

	HaltonSamplePattern m_haltonSamplePattern;

	struct FutureNames {
		MAKE_NAME(Scene);
	};
	unordered_map<string, future<void>> m_futures;

	exception_ptr m_exception;
	mutex m_exceptionMutex;

	vector<unique_ptr<Descriptor>> m_ImGUIDescriptors;

	unique_ptr<SHARC> m_SHARC;

	unique_ptr<Raytracing> m_raytracing;

	RTXDIResources m_RTXDIResources;
	unique_ptr<LightPreparation> m_lightPreparation;
	unique_ptr<RTXDI> m_RTXDI;

	unique_ptr<Streamline> m_streamline;

	bool m_isReflexLowLatencyAvailable{}, m_isDLSSFrameGenerationEnabled{};

	unique_ptr<NRD> m_NRD;
	unique_ptr<DenoisedComposition> m_denoisedComposition;

	unique_ptr<XeSS> m_XeSS;

	unique_ptr<ChromaticAberration> m_chromaticAberration;

	unique_ptr<Bloom> m_bloom;

	unique_ptr<ToneMapPostProcess> m_toneMapping[ToneMapPostProcess::Operator_Max];

	unique_ptr<BasicPostProcess> m_textureCopy;

	unique_ptr<SpriteBatch> m_textureAlphaBlending;

	unique_ptr<MipmapGeneration> m_mipmapGeneration;

	static constexpr DXGI_FORMAT m_HDRTextureFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
	struct TextureNames {
		MAKE_NAMEW(Color);
		MAKE_NAMEW(FinalColor);
		MAKE_NAMEW(PreviousLinearDepth);
		MAKE_NAMEW(LinearDepth);
		MAKE_NAMEW(NormalizedDepth);
		MAKE_NAMEW(MotionVectors);
		MAKE_NAMEW(PreviousBaseColorMetalness);
		MAKE_NAMEW(BaseColorMetalness);
		MAKE_NAMEW(Emission);
		MAKE_NAMEW(PreviousNormals);
		MAKE_NAMEW(Normals);
		MAKE_NAMEW(PreviousRoughness);
		MAKE_NAMEW(Roughness);
		MAKE_NAMEW(NormalRoughness);
		MAKE_NAMEW(NoisyDiffuse);
		MAKE_NAMEW(NoisySpecular);
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

	void CreateDeviceDependentResources() {
		const auto& deviceContext = m_deviceResources->GetDeviceContext();

		m_graphicsMemory = make_unique<GraphicsMemory>(deviceContext);

		{
			NVAPI_D3D12_RAYTRACING_THREAD_REORDERING_CAPS caps;
			m_isShaderExecutionReorderingAvailable = NvAPI_D3D12_GetRaytracingCaps(deviceContext, NVAPI_D3D12_RAYTRACING_CAPS_TYPE_THREAD_REORDERING, &caps, sizeof(caps)) == NVAPI_OK
				&& (caps & NVAPI_D3D12_RAYTRACING_THREAD_REORDERING_CAP_STANDARD)
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
			const auto CreateTexture = [&](LPCWSTR name, DXGI_FORMAT format, bool RTV = true, bool SRV = true, bool UAV = true) {
				Texture::CreationDesc creationDesc{
					.Format = format,
					.Width = static_cast<UINT>(outputSize.cx),
					.Height = static_cast<UINT>(outputSize.cy),
					.KeepInitialState = true
				};
				if (UAV) creationDesc.AsUnorderedAccess();
				if (RTV) creationDesc.AsRenderTarget();
				auto& texture = m_textures[name];
				texture = make_shared<Texture>(deviceContext, creationDesc);
				texture->GetNative()->SetName(name);
				if (SRV) texture->CreateSRV();
				if (UAV) texture->CreateUAV();
				if (RTV) texture->CreateRTV();
			};

			CreateTexture(TextureNames::Color, m_HDRTextureFormat);
			CreateTexture(TextureNames::FinalColor, m_HDRTextureFormat);
			CreateTexture(TextureNames::PreviousLinearDepth, DXGI_FORMAT_R32_FLOAT);
			CreateTexture(TextureNames::LinearDepth, DXGI_FORMAT_R32_FLOAT);
			CreateTexture(TextureNames::NormalizedDepth, DXGI_FORMAT_R32_FLOAT, false);
			CreateTexture(TextureNames::MotionVectors, DXGI_FORMAT_R16G16B16A16_FLOAT, false);
			CreateTexture(TextureNames::PreviousBaseColorMetalness, DXGI_FORMAT_R8G8B8A8_UNORM);
			CreateTexture(TextureNames::BaseColorMetalness, DXGI_FORMAT_R8G8B8A8_UNORM);
			CreateTexture(TextureNames::Emission, DXGI_FORMAT_R11G11B10_FLOAT, false);
			CreateTexture(TextureNames::PreviousNormals, DXGI_FORMAT_R16G16B16A16_SNORM);
			CreateTexture(TextureNames::Normals, DXGI_FORMAT_R16G16B16A16_SNORM);
			CreateTexture(TextureNames::PreviousRoughness, DXGI_FORMAT_R8_UNORM);
			CreateTexture(TextureNames::Roughness, DXGI_FORMAT_R8_UNORM);
			CreateTexture(TextureNames::NormalRoughness, NRD::ToDXGIFormat(GetLibraryDesc().normalEncoding), false);

			if (m_NRD = make_unique<NRD>(
				m_deviceResources->GetCommandList(),
				outputSize.cx, outputSize.cy,
				deviceResourcesCreationDesc.BackBufferCount,
				initializer_list<DenoiserDesc>{
					{ static_cast<Identifier>(NRDDenoiser::ReBLUR), Denoiser::REBLUR_DIFFUSE_SPECULAR },
					{ static_cast<Identifier>(NRDDenoiser::ReLAX), Denoiser::RELAX_DIFFUSE_SPECULAR }
			}
			);
				m_NRD->IsAvailable()) {
				CreateTexture(TextureNames::NoisyDiffuse, DXGI_FORMAT_R16G16B16A16_FLOAT, false, false);
				CreateTexture(TextureNames::NoisySpecular, DXGI_FORMAT_R16G16B16A16_FLOAT, false, false);
				CreateTexture(TextureNames::DenoisedDiffuse, DXGI_FORMAT_R16G16B16A16_FLOAT, false);
				CreateTexture(TextureNames::DenoisedSpecular, DXGI_FORMAT_R16G16B16A16_FLOAT, false);
				CreateTexture(TextureNames::Validation, DXGI_FORMAT_R8G8B8A8_UNORM);
			}

			m_XeSS = make_unique<XeSS>(deviceContext, xess_2d_t{ static_cast<uint32_t>(outputSize.cx), static_cast<uint32_t>(outputSize.cy) }, XESS_INIT_FLAG_INVERTED_DEPTH | XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE);

			SelectSuperResolutionUpscaler();
			SetSuperResolutionOptions();
		}

		m_cameraController.SetLens(XMConvertToRadians(g_graphicsSettings.Camera.HorizontalFieldOfView), static_cast<float>(outputSize.cx) / static_cast<float>(outputSize.cy));

		{
			const auto& IO = ImGui::GetIO();

			if (IO.BackendRendererUserData != nullptr) ImGui_ImplDX12_Shutdown();

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
		const auto isPCLAvailable = m_streamline->IsFeatureAvailable(kFeaturePCL);

		if (isPCLAvailable) ignore = m_streamline->SetPCLMarker(PCLMarker::eSimulationStart);

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
			m_camera.Jitter = g_graphicsSettings.Camera.IsJitterEnabled ? m_haltonSamplePattern.GetNext() : XMFLOAT2();
			m_camera.WorldToProjection = m_cameraController.GetWorldToProjection();
			m_camera.ProjectionToView = m_cameraController.GetProjectionToView();
			m_camera.ViewToWorld = m_cameraController.GetViewToWorld();

			m_deviceResources->GetCommandList().Copy(*m_GPUBuffers.Camera, initializer_list{ m_camera });
		}

		if (IsSceneReady()) UpdateScene();

		if (isPCLAvailable) ignore = m_streamline->SetPCLMarker(PCLMarker::eSimulationEnd);
	}

	void Render() {
		const auto isSceneReady = IsSceneReady();

		if (IsDLSSFrameGenerationEnabled()) {
			if (!m_isDLSSFrameGenerationEnabled && isSceneReady) SetDLSSFrameGenerationOptions(true);
			else if (m_isDLSSFrameGenerationEnabled && !isSceneReady) SetDLSSFrameGenerationOptions(false);
		}
		else if (m_isDLSSFrameGenerationEnabled) SetDLSSFrameGenerationOptions(false);

		auto& commandList = m_deviceResources->GetCommandList();

		const auto isPCLAvailable = m_streamline->IsFeatureAvailable(kFeaturePCL);

		if (isPCLAvailable) ignore = m_streamline->SetPCLMarker(PCLMarker::eRenderSubmitStart);

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
				if (m_resetHistory) ResetHistory();

				if (!m_scene->IsStatic()) {
					m_scene->CreateAccelerationStructures(commandList);
					commandList.CompactAccelerationStructures();
				}

				m_scene->CollectGarbage();

				RenderScene();

				PostProcessGraphics();

				m_resetHistory = false;

				{
					swap(m_textures.at(TextureNames::PreviousLinearDepth), m_textures.at(TextureNames::LinearDepth));
					swap(m_textures.at(TextureNames::PreviousBaseColorMetalness), m_textures.at(TextureNames::BaseColorMetalness));
					swap(m_textures.at(TextureNames::PreviousNormals), m_textures.at(TextureNames::Normals));
					swap(m_textures.at(TextureNames::PreviousRoughness), m_textures.at(TextureNames::Roughness));
				}
			}

			if (m_UIStates.IsVisible) RenderUI();
		}

		if (isPCLAvailable) {
			ignore = m_streamline->SetPCLMarker(PCLMarker::eRenderSubmitEnd);

			ignore = m_streamline->SetPCLMarker(PCLMarker::ePresentStart);
		}

		m_deviceResources->Present();

		if (isPCLAvailable) ignore = m_streamline->SetPCLMarker(PCLMarker::ePresentEnd);
	}

	void OnRenderSizeChanged() {
		const auto outputSize = GetOutputSize();

		m_resetHistory = true;

		m_haltonSamplePattern = HaltonSamplePattern(static_cast<uint32_t>(ceil(8 * (static_cast<float>(outputSize.cx) / static_cast<float>(m_renderSize.x)) * (static_cast<float>(outputSize.cy) / static_cast<float>(m_renderSize.y)))));

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
		commandList.Clear(*m_textures.at(TextureNames::PreviousBaseColorMetalness));
		commandList.Clear(*m_textures.at(TextureNames::PreviousNormals));
		commandList.Clear(*m_textures.at(TextureNames::PreviousRoughness));

		if (g_graphicsSettings.Raytracing.RTXGI.Technique == RTXGITechnique::SHARC) {
			commandList.Clear(*m_SHARC->GPUBuffers.HashEntries);
			commandList.Clear(*m_SHARC->GPUBuffers.HashCopyOffset);
			commandList.Clear(*m_SHARC->GPUBuffers.PreviousVoxelData);
		}
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

			if (!m_exception) m_exception = current_exception();
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

			DXGI_ADAPTER_DESC adapterDesc;
			ThrowIfFailed(m_deviceResources->GetAdapter()->GetDesc(&adapterDesc));
			m_streamline = make_unique<Streamline>(&adapterDesc.AdapterLuid, sizeof(adapterDesc.AdapterLuid), m_deviceResources->GetCommandList(), 0);

			if (m_streamline->IsFeatureAvailable(kFeatureReflex)) {
				ReflexState state;
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

		m_chromaticAberration = make_unique<ChromaticAberration>(deviceContext);

		m_bloom = make_unique<Bloom>(deviceContext);

		{
			const RenderTargetState renderTargetState(m_HDRTextureFormat, DXGI_FORMAT_UNKNOWN);

			for (const auto Operator : {
				ToneMapPostProcess::Saturate,
				ToneMapPostProcess::Reinhard,
				ToneMapPostProcess::ACESFilmic,
				ToneMapPostProcess::Operator_Max }) {
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
		const auto CreateBuffer = [&]<typename T>(T, auto & buffer, UINT64 capacity) {
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
		if (const auto gamepadState = m_inputDevices.Gamepad->GetState(0); gamepadState.IsConnected()) gamepadStateTracker.Update(gamepadState);
		else gamepadStateTracker.Reset();

		auto& keyboardStateTracker = m_inputDeviceStateTrackers.Keyboard;
		keyboardStateTracker.Update(m_inputDevices.Keyboard->GetState());

		auto& mouseStateTracker = m_inputDeviceStateTrackers.Mouse;
		mouseStateTracker.Update(m_inputDevices.Mouse->GetState());
		m_inputDevices.Mouse->ResetScrollWheelValue();

		const auto isUIVisible = m_UIStates.IsVisible;

		{
			if (gamepadStateTracker.menu == GamepadButtonState::PRESSED) m_UIStates.IsVisible = !m_UIStates.IsVisible;
			if (keyboardStateTracker.IsKeyPressed(Key::Escape)) m_UIStates.IsVisible = !m_UIStates.IsVisible;
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
			if (IO.WantCaptureKeyboard) m_UIStates.HasFocus = true;
			if (IO.WantCaptureMouse && !(IO.ConfigFlags & ImGuiConfigFlags_NoMouse)) m_UIStates.HasFocus = true;
			else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && isSceneReady) m_UIStates.HasFocus = false;
			if (m_UIStates.HasFocus) {
				IO.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;

				m_inputDevices.Mouse->SetMode(Mouse::MODE_ABSOLUTE);
			}
			else IO.ConfigFlags |= ImGuiConfigFlags_NoMouse;
		}

		if (isSceneReady && (!m_UIStates.IsVisible || !m_UIStates.HasFocus)) {
			m_inputDevices.Mouse->SetMode(Mouse::MODE_RELATIVE);

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
			if (m_inputDeviceStateTrackers.Gamepad.view == GamepadButtonState::PRESSED) ResetCamera();

			int movementSpeedIncrement = 0;
			if (gamepadState.IsDPadUpPressed()) movementSpeedIncrement++;
			if (gamepadState.IsDPadDownPressed()) movementSpeedIncrement--;
			if (movementSpeedIncrement) speedSettings.Movement = clamp(speedSettings.Movement + elapsedSeconds * static_cast<float>(movementSpeedIncrement) * 12, 0.0f, speedSettings.MaxMovement);

			const auto movementSpeed = elapsedSeconds * speedSettings.Movement;
			displacement.x += gamepadState.thumbSticks.leftX * movementSpeed;
			displacement.z += gamepadState.thumbSticks.leftY * movementSpeed;

			const auto rotationSpeed = elapsedSeconds * XM_2PI * speedSettings.Rotation;
			yaw += gamepadState.thumbSticks.rightX * rotationSpeed;
			pitch += gamepadState.thumbSticks.rightY * rotationSpeed;
		}

		if (mouseState.positionMode == Mouse::MODE_RELATIVE) {
			if (m_inputDeviceStateTrackers.Keyboard.IsKeyPressed(Key::Home)) ResetCamera();

			if (mouseState.scrollWheelValue) speedSettings.Movement = clamp(speedSettings.Movement + static_cast<float>(mouseState.scrollWheelValue) * 0.008f, 0.0f, speedSettings.MaxMovement);

			const auto movementSpeed = elapsedSeconds * speedSettings.Movement;
			if (keyboardState.A) displacement.x -= movementSpeed;
			if (keyboardState.D) displacement.x += movementSpeed;
			if (keyboardState.W) displacement.z += movementSpeed;
			if (keyboardState.S) displacement.z -= movementSpeed;

			const auto rotationSpeed = 4e-4f * XM_2PI * speedSettings.Rotation;
			yaw += static_cast<float>(mouseState.x) * rotationSpeed;
			pitch += static_cast<float>(-mouseState.y) * rotationSpeed;
		}

		if (pitch == 0) {
			if (displacement == Vector3() && yaw == 0) return;
		}
		else if (const auto angle = XM_PIDIV2 - abs(-m_cameraController.GetRotation().ToEuler().x + pitch); angle <= 0) pitch = copysign(max(0.0f, angle - 0.1f), pitch);

		m_cameraController.Translate(m_cameraController.GetNormalizedRightDirection() * displacement.x + m_cameraController.GetNormalizedUpDirection() * displacement.y + m_cameraController.GetNormalizedForwardDirection() * displacement.z);
		m_cameraController.Rotate(yaw, pitch);
	}

	void UpdateScene() {
		m_scene->Tick(m_stepTimer.GetElapsedSeconds(), m_inputDeviceStateTrackers.Gamepad, m_inputDeviceStateTrackers.Keyboard, m_inputDeviceStateTrackers.Mouse);

		auto& commandList = m_deviceResources->GetCommandList();

		{
			SceneData sceneData{
				.IsStatic = m_scene->IsStatic(),
				.EnvironmentLightColor = m_scene->EnvironmentLightColor,
				.EnvironmentColor = m_scene->EnvironmentColor,
			};

			if (m_scene->EnvironmentLightTexture.Texture) {
				sceneData.IsEnvironmentLightTextureCubeMap = m_scene->EnvironmentLightTexture.Texture->IsCubeMap();
				sceneData.ResourceDescriptorIndices.EnvironmentLightTexture = m_scene->EnvironmentLightTexture.Texture->GetSRVDescriptor();
				XMStoreFloat3x4(&sceneData.EnvironmentLightTextureTransform, m_scene->EnvironmentLightTexture.Transform());
			}
			if (m_scene->EnvironmentTexture.Texture) {
				sceneData.IsEnvironmentTextureCubeMap = m_scene->EnvironmentTexture.Texture->IsCubeMap();
				sceneData.ResourceDescriptorIndices.EnvironmentTexture = m_scene->EnvironmentTexture.Texture->GetSRVDescriptor();
				XMStoreFloat3x4(&sceneData.EnvironmentTextureTransform, m_scene->EnvironmentTexture.Transform());
			}

			commandList.Copy(*m_GPUBuffers.SceneData, initializer_list{ sceneData });
		}

		vector<InstanceData> instanceData(size(m_scene->GetInstanceData()));
		vector<ObjectData> objectData(m_scene->GetObjectCount());
		for (UINT instanceIndex = 0; const auto & renderObject : m_scene->RenderObjects) {
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

			_objectData.ResourceDescriptorIndices.Mesh = {
				.Vertices = mesh->Vertices->GetSRVDescriptor(BufferSRVType::Raw),
				.Indices = mesh->Indices->GetSRVDescriptor(BufferSRVType::Structured)
			};

			for (size_t i = 0; const auto & texture : renderObject.Textures) {
				if (texture) {
					const auto index = texture->GetSRVDescriptor().GetIndex();
					switch (auto& indices = _objectData.ResourceDescriptorIndices.Textures; static_cast<TextureMapType>(i)) {
						case TextureMapType::BaseColor: indices.BaseColor = index; break;
						case TextureMapType::EmissiveColor: indices.EmissiveColor = index; break;
						case TextureMapType::Metallic: indices.Metallic = index; break;
						case TextureMapType::Roughness: indices.Roughness = index; break;
						case TextureMapType::AmbientOcclusion: indices.AmbientOcclusion = index; break;
						case TextureMapType::Transmission: indices.Transmission = index; break;
						case TextureMapType::Opacity: indices.Opacity = index; break;
						case TextureMapType::Normal: indices.Normal = index; break;
						default: Throw<out_of_range>("Unsupported texture map type");
					}
					_objectData.Material.HasTexture = true;
				}
				i++;
			}
		}
		commandList.Copy(*m_GPUBuffers.InstanceData, instanceData);
		commandList.Copy(*m_GPUBuffers.ObjectData, objectData);
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

		m_RTXDIResources.Context->setLightBufferParams(m_lightPreparation->GetLightBufferParameters());

		const auto& settings = g_graphicsSettings.Raytracing.RTXDI.ReSTIRDI;

		{
			auto& context = m_RTXDIResources.Context->getReGIRContext();

			auto dynamicParameters = context.getReGIRDynamicParameters();
			dynamicParameters.regirCellSize = settings.ReGIR.Cell.Size;
			dynamicParameters.center = reinterpret_cast<const rtxdi::float3&>(m_cameraController.GetPosition());
			dynamicParameters.regirNumBuildSamples = settings.ReGIR.BuildSamples;
			context.setDynamicParameters(dynamicParameters);
		}

		{
			auto& context = m_RTXDIResources.Context->getReSTIRDIContext();

			context.setFrameIndex(m_stepTimer.GetFrameCount() - 1);

			auto initialSamplingParameters = context.getInitialSamplingParameters();
			initialSamplingParameters.numPrimaryLocalLightSamples = settings.InitialSampling.LocalLight.Samples;
			initialSamplingParameters.numPrimaryBrdfSamples = settings.InitialSampling.BRDFSamples;
			initialSamplingParameters.brdfCutoff = 0;
			initialSamplingParameters.localLightSamplingMode = settings.InitialSampling.LocalLight.Mode;
			context.setInitialSamplingParameters(initialSamplingParameters);

			auto temporalResamplingParameters = context.getTemporalResamplingParameters();
			temporalResamplingParameters.temporalBiasCorrection = settings.TemporalResampling.BiasCorrectionMode;
			temporalResamplingParameters.enableBoilingFilter = settings.TemporalResampling.BoilingFilter.IsEnabled;
			temporalResamplingParameters.boilingFilterStrength = settings.TemporalResampling.BoilingFilter.Strength;
			context.setTemporalResamplingParameters(temporalResamplingParameters);

			auto spatialResamplingParameters = context.getSpatialResamplingParameters();
			spatialResamplingParameters.spatialBiasCorrection = settings.SpatialResampling.BiasCorrectionMode;
			spatialResamplingParameters.numSpatialSamples = settings.SpatialResampling.Samples;
			context.setSpatialResamplingParameters(spatialResamplingParameters);
		}
	}

	void RenderScene() {
		auto& commandList = m_deviceResources->GetCommandList();

		const auto& raytracingSettings = g_graphicsSettings.Raytracing;

		const auto& ReSTIRDISettings = raytracingSettings.RTXDI.ReSTIRDI;
		auto isReSTIRDIEnabled = ReSTIRDISettings.IsEnabled;
		if (isReSTIRDIEnabled) {
			PrepareReSTIRDI();
			isReSTIRDIEnabled &= m_RTXDIResources.LightInfo != nullptr;
		}

		const NRDSettings NRDSettings{
			.Denoiser = m_NRD->IsAvailable() ? g_graphicsSettings.PostProcessing.NRD.Denoiser : NRDDenoiser::None,
			.HitDistanceParameters = reinterpret_cast<const XMFLOAT4&>(ReblurSettings().hitDistanceParameters)
		};

		const auto
			color = m_textures.at(TextureNames::Color).get(),
			linearDepth = m_textures.at(TextureNames::LinearDepth).get(),
			motionVectors = m_textures.at(TextureNames::MotionVectors).get(),
			baseColorMetalness = m_textures.at(TextureNames::BaseColorMetalness).get(),
			normals = m_textures.at(TextureNames::Normals).get(),
			roughness = m_textures.at(TextureNames::Roughness).get(),
			noisyDiffuse = m_textures.at(TextureNames::NoisyDiffuse).get(),
			noisySpecular = m_textures.at(TextureNames::NoisySpecular).get();

		const auto& scene = m_scene->GetTopLevelAccelerationStructure();

		{
			m_raytracing->GPUBuffers = {
				.SceneData = m_GPUBuffers.SceneData.get(),
				.Camera = m_GPUBuffers.Camera.get(),
				.InstanceData = m_GPUBuffers.InstanceData.get(),
				.ObjectData = m_GPUBuffers.ObjectData.get()
			};

			m_raytracing->Textures = {
				.Color = color,
				.LinearDepth = linearDepth,
				.NormalizedDepth = m_textures.at(TextureNames::NormalizedDepth).get(),
				.MotionVectors = motionVectors,
				.BaseColorMetalness = baseColorMetalness,
				.Emission = m_textures.at(TextureNames::Emission).get(),
				.Normals = normals,
				.Roughness = roughness,
				.NormalRoughness = m_textures.at(TextureNames::NormalRoughness).get(),
				.NoisyDiffuse = noisyDiffuse,
				.NoisySpecular = noisySpecular
			};

			m_raytracing->SetConstants({
				.RenderSize = m_renderSize,
				.FrameIndex = m_stepTimer.GetFrameCount() - 1,
				.Bounces = raytracingSettings.Bounces,
				.SamplesPerPixel = raytracingSettings.SamplesPerPixel,
				.IsRussianRouletteEnabled = raytracingSettings.IsRussianRouletteEnabled,
				.IsShaderExecutionReorderingEnabled = IsShaderExecutionReorderingEnabled(),
				.IsSecondarySurfaceEmissionIncluded = !isReSTIRDIEnabled,
				.NRD = NRDSettings
				});

			switch (raytracingSettings.RTXGI.Technique)
			{
				case RTXGITechnique::None: m_raytracing->Render(commandList, scene); break;

				case RTXGITechnique::SHARC:
				{
					if (raytracingSettings.Bounces) {
						commandList.Clear(*m_SHARC->GPUBuffers.VoxelData);

						m_SHARC->GPUBuffers.Camera = m_GPUBuffers.Camera.get();

						Raytracing::SHARCSettings SHARCSettings{
							.DownscaleFactor = raytracingSettings.RTXGI.SHARC.DownscaleFactor,
							.RoughnessThreshold = raytracingSettings.RTXGI.SHARC.RoughnessThreshold,
							.IsHashGridVisualizationEnabled = raytracingSettings.RTXGI.SHARC.IsHashGridVisualizationEnabled
						};
						SHARCSettings.SceneScale = raytracingSettings.RTXGI.SHARC.SceneScale;
						m_raytracing->Render(commandList, scene, *m_SHARC, SHARCSettings);

						swap(m_SHARC->GPUBuffers.PreviousVoxelData, m_SHARC->GPUBuffers.VoxelData);
					}
					else m_raytracing->Render(commandList, scene);
				}
				break;
			}
		}

		if (!m_scene->GetObjectCount()) return;

		if (isReSTIRDIEnabled) {
			m_RTXDI->GPUBuffers = {
				.Camera = m_GPUBuffers.Camera.get(),
				.ObjectData = m_GPUBuffers.ObjectData.get()
			};

			m_RTXDI->Textures = {
				.PreviousLinearDepth = m_textures.at(TextureNames::PreviousLinearDepth).get(),
				.LinearDepth = linearDepth,
				.MotionVectors = motionVectors,
				.PreviousBaseColorMetalness = m_textures.at(TextureNames::PreviousBaseColorMetalness).get(),
				.BaseColorMetalness = baseColorMetalness,
				.PreviousNormals = m_textures.at(TextureNames::PreviousNormals).get(),
				.Normals = normals,
				.PreviousRoughness = m_textures.at(TextureNames::PreviousRoughness).get(),
				.Roughness = roughness,
				.Color = color,
				.NoisyDiffuse = noisyDiffuse,
				.NoisySpecular = noisySpecular
			};

			m_RTXDI->SetConstants(m_RTXDIResources, ReSTIRDISettings.ReGIR.Cell.IsVisualizationEnabled, NRDSettings);

			m_RTXDI->Render(commandList, scene);
		}
	}

	bool IsDLSSFrameGenerationEnabled() const { return g_graphicsSettings.PostProcessing.IsDLSSFrameGenerationEnabled && m_streamline->IsFeatureAvailable(kFeatureDLSS_G) && IsReflexEnabled(); }
	bool IsDLSSSuperResolutionEnabled() const { return g_graphicsSettings.PostProcessing.SuperResolution.Upscaler == Upscaler::DLSS && m_streamline->IsFeatureAvailable(kFeatureDLSS); }
	bool IsNISEnabled() const { return g_graphicsSettings.PostProcessing.NIS.IsEnabled && m_streamline->IsFeatureAvailable(kFeatureNIS); }
	bool IsNRDEnabled() const { return g_graphicsSettings.PostProcessing.NRD.Denoiser != NRDDenoiser::None && m_NRD->IsAvailable(); }
	bool IsReflexEnabled() const { return g_graphicsSettings.ReflexMode != ReflexMode::eOff && m_streamline->IsFeatureAvailable(kFeatureReflex); }
	bool IsShaderExecutionReorderingEnabled() const { return m_isShaderExecutionReorderingAvailable && g_graphicsSettings.Raytracing.IsShaderExecutionReorderingEnabled; }
	bool IsXeSSSuperResolutionEnabled() const { return g_graphicsSettings.PostProcessing.SuperResolution.Upscaler == Upscaler::XeSS && m_XeSS->IsAvailable(); }

	static void SetReflexOptions() {
		ReflexOptions options;
		options.mode = g_graphicsSettings.ReflexMode;
		ignore = slReflexSetOptions(options);
	}

	auto CreateResourceTagDesc(BufferType type, const Texture& texture, bool isRenderSize = true, ResourceLifecycle lifecycle = ResourceLifecycle::eValidUntilPresent) const {
		ResourceTagDesc resourceTagDesc{
			.Type = type,
			.Resource = Resource(sl::ResourceType::eTex2d, texture, texture.GetState()),
			.Lifecycle = lifecycle
		};
		if (isRenderSize) resourceTagDesc.Extent = { .width = m_renderSize.x, .height = m_renderSize.y };
		return resourceTagDesc;
	}

	void SelectSuperResolutionUpscaler() {
		if (auto& upscaler = g_graphicsSettings.PostProcessing.SuperResolution.Upscaler;
			upscaler == Upscaler::DLSS) {
			if (!m_streamline->IsFeatureAvailable(kFeatureDLSS)) upscaler = m_XeSS->IsAvailable() ? Upscaler::XeSS : Upscaler::None;
		}
		else if (upscaler == Upscaler::XeSS) {
			if (!m_XeSS->IsAvailable()) upscaler = m_streamline->IsFeatureAvailable(kFeatureDLSS) ? Upscaler::DLSS : Upscaler::None;
		}
	}

	void SetSuperResolutionOptions() {
		const auto outputSize = GetOutputSize();

		const auto SelectSuperResolutionMode = [&] {
			if (g_graphicsSettings.PostProcessing.SuperResolution.Mode != SuperResolutionMode::Auto) return g_graphicsSettings.PostProcessing.SuperResolution.Mode;
			const auto pixelCount = outputSize.cx * outputSize.cy;
			if (pixelCount <= 1280 * 800) return SuperResolutionMode::Native;
			if (pixelCount <= 1920 * 1200) return SuperResolutionMode::Quality;
			if (pixelCount <= 2560 * 1600) return SuperResolutionMode::Balanced;
			if (pixelCount <= 3840 * 2400) return SuperResolutionMode::Performance;
			return SuperResolutionMode::UltraPerformance;
		};

		switch (g_graphicsSettings.PostProcessing.SuperResolution.Upscaler) {
			case Upscaler::None: m_renderSize = XMUINT2(outputSize.cx, outputSize.cy); break;

			case Upscaler::DLSS:
			{
				DLSSOptions options;
				switch (auto& mode = options.mode; SelectSuperResolutionMode()) {
					case SuperResolutionMode::Native: mode = DLSSMode::eDLAA; break;
					case SuperResolutionMode::Quality: mode = DLSSMode::eMaxQuality; break;
					case SuperResolutionMode::Balanced: mode = DLSSMode::eBalanced; break;
					case SuperResolutionMode::Performance: mode = DLSSMode::eMaxPerformance; break;
					case SuperResolutionMode::UltraPerformance: mode = DLSSMode::eUltraPerformance; break;
					default: throw;
				}
				options.outputWidth = outputSize.cx;
				options.outputHeight = outputSize.cy;
				DLSSOptimalSettings optimalSettings;
				ignore = slDLSSGetOptimalSettings(options, optimalSettings);
				ignore = m_streamline->SetConstants(options);
				m_renderSize = XMUINT2(optimalSettings.optimalRenderWidth, optimalSettings.optimalRenderHeight);
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
		DLSSGOptions options;
		options.mode = isEnabled ? DLSSGMode::eAuto : DLSSGMode::eOff;
		ignore = m_streamline->SetConstants(options);
		m_isDLSSFrameGenerationEnabled = isEnabled;
	}

	void PostProcessGraphics() {
		const auto& postProcessingSettings = g_graphicsSettings.PostProcessing;

		PrepareStreamline();

		const auto isNRDEnabled = IsNRDEnabled();

		if (isNRDEnabled) ProcessNRD();

		auto inColor = m_textures.at(TextureNames::Color).get(), outColor = m_textures.at(TextureNames::FinalColor).get();

		if (IsDLSSSuperResolutionEnabled()) {
			ProcessDLSSSuperResolution();

			swap(inColor, outColor);
		}
		else if (IsXeSSSuperResolutionEnabled()) {
			ProcessXeSSSuperResolution();

			swap(inColor, outColor);
		}

		if (IsNISEnabled()) {
			ProcessNIS(*inColor, *outColor);

			swap(inColor, outColor);
		}

		if (postProcessingSettings.IsChromaticAberrationEnabled) {
			ProcessChromaticAberration(*inColor, *outColor);

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

		if (IsDLSSFrameGenerationEnabled()) ProcessDLSSFrameGeneration(*inColor);

		if (isNRDEnabled && postProcessingSettings.NRD.IsValidationOverlayEnabled) {
			AlphaBlendTexture(*m_textures.at(TextureNames::Validation));
		}
	}

	void PrepareStreamline() {
		Constants constants;
		reinterpret_cast<XMFLOAT4X4&>(constants.cameraViewToClip) = m_cameraController.GetViewToProjection();
		recalculateCameraMatrices(constants, reinterpret_cast<const float4x4&>(m_camera.PreviousViewToWorld), reinterpret_cast<const float4x4&>(m_camera.PreviousViewToProjection));
		constants.jitterOffset = { -m_camera.Jitter.x, -m_camera.Jitter.y };
		constants.mvecScale = { 1 / static_cast<float>(m_renderSize.x), 1 / static_cast<float>(m_renderSize.y) };
		constants.cameraPinholeOffset = { 0, 0 };
		reinterpret_cast<XMFLOAT3&>(constants.cameraPos) = m_cameraController.GetPosition();
		reinterpret_cast<XMFLOAT3&>(constants.cameraUp) = m_cameraController.GetUpDirection();
		reinterpret_cast<XMFLOAT3&>(constants.cameraRight) = m_cameraController.GetRightDirection();
		reinterpret_cast<XMFLOAT3&>(constants.cameraFwd) = m_cameraController.GetForwardDirection();
		constants.cameraNear = m_cameraController.GetNearDepth();
		constants.cameraFar = m_cameraController.GetFarDepth();
		constants.cameraFOV = m_cameraController.GetVerticalFieldOfView();
		constants.cameraAspectRatio = m_cameraController.GetAspectRatio();
		constants.depthInverted = Boolean::eTrue;
		constants.cameraMotionIncluded = Boolean::eTrue;
		constants.motionVectors3D = Boolean::eFalse;
		constants.reset = m_resetHistory ? Boolean::eTrue : Boolean::eFalse;
		ignore = m_streamline->SetConstants(constants);
	}

	void ProcessNRD() {
		auto& commandList = m_deviceResources->GetCommandList();

		const auto& NRDSettings = g_graphicsSettings.PostProcessing.NRD;

		auto
			& linearDepth = *m_textures.at(TextureNames::LinearDepth),
			& baseColorMetalness = *m_textures.at(TextureNames::BaseColorMetalness),
			& normalRoughness = *m_textures.at(TextureNames::NormalRoughness),
			& denoisedDiffuse = *m_textures.at(TextureNames::DenoisedDiffuse),
			& denoisedSpecular = *m_textures.at(TextureNames::DenoisedSpecular),
			& color = *m_textures.at(TextureNames::Color);

		{
			m_NRD->NewFrame();

			const auto Tag = [&](nrd::ResourceType resourceType, Texture& texture) { m_NRD->Tag(resourceType, texture); };
			Tag(nrd::ResourceType::IN_VIEWZ, linearDepth);
			Tag(nrd::ResourceType::IN_MV, *m_textures.at(TextureNames::MotionVectors));
			Tag(nrd::ResourceType::IN_BASECOLOR_METALNESS, baseColorMetalness);
			Tag(nrd::ResourceType::IN_NORMAL_ROUGHNESS, normalRoughness);
			Tag(nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST, *m_textures.at(TextureNames::NoisyDiffuse));
			Tag(nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST, *m_textures.at(TextureNames::NoisySpecular));
			Tag(nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST, denoisedDiffuse);
			Tag(nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST, denoisedSpecular);
			Tag(nrd::ResourceType::OUT_VALIDATION, *m_textures.at(TextureNames::Validation));

			const auto outputSize = GetOutputSize();
			CommonSettings commonSettings{
				.motionVectorScale{ 1 / static_cast<float>(m_renderSize.x), 1 / static_cast<float>(m_renderSize.y), 1 },
				.resourceSize{ static_cast<uint16_t>(outputSize.cx), static_cast<uint16_t>(outputSize.cy) },
				.rectSize{ static_cast<uint16_t>(m_renderSize.x), static_cast<uint16_t>(m_renderSize.y) },
				.frameIndex = m_stepTimer.GetFrameCount() - 1,
				.accumulationMode = m_resetHistory ? AccumulationMode::CLEAR_AND_RESTART : AccumulationMode::CONTINUE,
				.isBaseColorMetalnessAvailable = true,
				.enableValidation = NRDSettings.IsValidationOverlayEnabled
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

			const auto denoiser = static_cast<Identifier>(NRDSettings.Denoiser);
			if (NRDSettings.Denoiser == NRDDenoiser::ReBLUR) {
				ignore = m_NRD->SetConstants(
					denoiser,
					ReblurSettings{
						.hitDistanceReconstructionMode = HitDistanceReconstructionMode::AREA_3X3,
						.enableAntiFirefly = true
					}
				);
			}
			else if (NRDSettings.Denoiser == NRDDenoiser::ReLAX) {
				ignore = m_NRD->SetConstants(
					denoiser,
					RelaxSettings{
						.hitDistanceReconstructionMode = HitDistanceReconstructionMode::AREA_3X3,
						.enableAntiFirefly = true
					}
				);
			}
			m_NRD->Denoise(initializer_list<Identifier>{ denoiser });
		}

		m_denoisedComposition->GPUBuffers = { .Camera = m_GPUBuffers.Camera.get() };

		m_denoisedComposition->Textures = {
			.LinearDepth = &linearDepth,
			.BaseColorMetalness = &baseColorMetalness,
			.Emission = m_textures.at(TextureNames::Emission).get(),
			.NormalRoughness = &normalRoughness,
			.DenoisedDiffuse = &denoisedDiffuse,
			.DenoisedSpecular = &denoisedSpecular,
			.Color = &color
		};

		m_denoisedComposition->Process(commandList, { .RenderSize = m_renderSize, .NRDDenoiser = NRDSettings.Denoiser });
	}

	void ProcessDLSSSuperResolution() {
		ResourceTagDesc resourceTagDescs[]{
			CreateResourceTagDesc(kBufferTypeDepth, *m_textures.at(TextureNames::NormalizedDepth)),
			CreateResourceTagDesc(kBufferTypeMotionVectors, *m_textures.at(TextureNames::MotionVectors)),
			CreateResourceTagDesc(kBufferTypeScalingInputColor, *m_textures.at(TextureNames::Color)),
			CreateResourceTagDesc(kBufferTypeScalingOutputColor, *m_textures.at(TextureNames::FinalColor), false)
		};
		ignore = m_streamline->EvaluateFeature(kFeatureDLSS, CreateResourceTags(resourceTagDescs));
	}

	void ProcessDLSSFrameGeneration(Texture& inColor) {
		ResourceTagDesc resourceTagDescs[]{
			CreateResourceTagDesc(kBufferTypeDepth, *m_textures.at(TextureNames::NormalizedDepth)),
			CreateResourceTagDesc(kBufferTypeMotionVectors, *m_textures.at(TextureNames::MotionVectors)),
			CreateResourceTagDesc(kBufferTypeHUDLessColor, inColor, false)
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
			{ XeSSResourceType::Velocity, *m_textures.at(TextureNames::MotionVectors) },
			{ XeSSResourceType::Color, *m_textures.at(TextureNames::Color) },
			{ XeSSResourceType::Output, *m_textures.at(TextureNames::FinalColor), D3D12_RESOURCE_STATE_UNORDERED_ACCESS }
			}) {
			commandList.SetState(Texture, State);
			m_XeSS->Tag(Type, Texture);
		}

		ignore = m_XeSS->Execute(commandList);
	}

	void ProcessNIS(Texture& inColor, Texture& outColor) {
		NISOptions NISOptions;
		NISOptions.mode = NISMode::eSharpen;
		NISOptions.sharpness = g_graphicsSettings.PostProcessing.NIS.Sharpness;
		ignore = m_streamline->SetConstants(NISOptions);

		ResourceTagDesc resourceTagDescs[]{
			CreateResourceTagDesc(kBufferTypeScalingInputColor, inColor, false),
			CreateResourceTagDesc(kBufferTypeScalingOutputColor, outColor, false)
		};
		ignore = m_streamline->EvaluateFeature(kFeatureNIS, CreateResourceTags(resourceTagDescs));
	}

	void ProcessChromaticAberration(Texture& inColor, Texture& outColor) {
		m_chromaticAberration->Textures = {
			.Input = &inColor,
			.Output = &outColor
		};

		m_chromaticAberration->Process(m_deviceResources->GetCommandList());
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
		else toneMapping.SetExposure(toneMappingSettings.NonHDR.Exposure);

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

		if (m_UIStates.IsSettingsWindowOpen) RenderSettingsWindow();

		RenderPopupModalWindow(popupModalName);

		if (IsSceneLoading()) RenderLoadingSceneWindow();

		ImGui::Render();
		ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), m_deviceResources->GetCommandList());
	}

	string RenderMainMenuBar() {
		string popupModalName;
		if (ImGuiEx::MainMenuBar mainMenuBar; mainMenuBar) {
			if (ImGui::GetFrameCount() == 1) ImGui::SetKeyboardFocusHere();

			const auto PopupModal = [&](LPCSTR name) { if (ImGui::MenuItem(name)) popupModalName = name; };

			if (ImGuiEx::Menu menu("File"); menu) {
				if (ImGui::MenuItem("Exit")) PostQuitMessage(ERROR_SUCCESS);
			}

			if (ImGuiEx::Menu menu("View"); menu) m_UIStates.IsSettingsWindowOpen |= ImGui::MenuItem("Settings");

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
						m_futures["WindowSetting"] = async(launch::deferred, [&] { ThrowIfFailed(m_windowModeHelper.Apply()); });
					}
				}

				{
					auto isEnabled = m_deviceResources->IsHDREnabled();
					if (const ImGuiEx::Enablement enablement(m_deviceResources->IsHDRSupported());
						ImGui::Checkbox("HDR", &isEnabled)) {
						g_graphicsSettings.IsHDREnabled = isEnabled;

						m_futures["HDRSetting"] = async(launch::deferred, [&] { m_deviceResources->RequestHDR(g_graphicsSettings.IsHDREnabled); });
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
					ImGuiEx::Combo<ReflexMode>(
						"NVIDIA Reflex",
						{ ReflexMode::eOff, ReflexMode::eLowLatency, ReflexMode::eLowLatencyWithBoost },
						m_isReflexLowLatencyAvailable ? g_graphicsSettings.ReflexMode : ReflexMode::eOff,
						g_graphicsSettings.ReflexMode,
						static_cast<string(*)(ReflexMode)>(ToString)
						)) {
					m_futures["ReflexSetting"] = async(launch::deferred, [&] { SetReflexOptions(); });
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

					m_resetHistory |= ImGui::SliderInt("Samples/Pixel", reinterpret_cast<int*>(&raytracingSettings.SamplesPerPixel), 1, raytracingSettings.MaxSamplesPerPixel, "%u", ImGuiSliderFlags_AlwaysClamp);

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

					{
						const auto isAvailable = m_NRD->IsAvailable();
						const ImGuiEx::Enablement enablement(isAvailable);
						if (ImGuiEx::TreeNode treeNode("NVIDIA Real-Time Denoisers", isAvailable ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None); treeNode) {
							auto& NRDSettings = postProcessingSetttings.NRD;

							m_resetHistory |= ImGuiEx::Combo<NRDDenoiser>(
								"Denoiser",
								{ NRDDenoiser::None, NRDDenoiser::ReBLUR, NRDDenoiser::ReLAX },
								NRDSettings.Denoiser,
								NRDSettings.Denoiser,
								static_cast<string(*)(NRDDenoiser)>(ToString)
								);

							if (NRDSettings.Denoiser != NRDDenoiser::None) {
								ImGui::Checkbox("Validation Overlay", &NRDSettings.IsValidationOverlayEnabled);
							}
						}
					}

					if (ImGuiEx::TreeNode treeNode("Super Resolution", ImGuiTreeNodeFlags_DefaultOpen); treeNode) {
						auto& superResolutionSettings = postProcessingSetttings.SuperResolution;

						auto isChanged = false;

						{
							const auto IsSelectable = [&](Upscaler upscaler) {
								return upscaler == Upscaler::None
									|| (upscaler == Upscaler::DLSS && m_streamline->IsFeatureAvailable(kFeatureDLSS))
									|| (upscaler == Upscaler::XeSS && m_XeSS->IsAvailable()) ?
									ImGuiSelectableFlags_None : ImGuiSelectableFlags_Disabled;
							};
							isChanged |= ImGuiEx::Combo<Upscaler>(
								"Upscaler",
								{ Upscaler::None, Upscaler::DLSS, Upscaler::XeSS },
								superResolutionSettings.Upscaler,
								superResolutionSettings.Upscaler,
								static_cast<string(*)(Upscaler)>(ToString),
								IsSelectable
								);
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
							m_futures["SuperResolutionSetting"] = async(launch::deferred, [&] { SetSuperResolutionOptions(); });
						}
					}

					if (IsReflexEnabled()) {
						auto isEnabled = IsDLSSFrameGenerationEnabled();
						if (const ImGuiEx::Enablement enablement(m_streamline->IsFeatureAvailable(kFeatureDLSS_G));
							ImGui::Checkbox("NVIDIA DLSS Frame Generation", &isEnabled)) {
							postProcessingSetttings.IsDLSSFrameGenerationEnabled = isEnabled;
						}
					}

					{
						const auto isAvailable = m_streamline->IsFeatureAvailable(kFeatureNIS);
						const ImGuiEx::Enablement enablement(isAvailable);
						if (ImGuiEx::TreeNode treeNode("NVIDIA Image Scaling", isAvailable ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None); treeNode) {
							auto& NISSettings = postProcessingSetttings.NIS;

							ImGui::Checkbox("Enable", &NISSettings.IsEnabled);

							if (NISSettings.IsEnabled) {
								ImGui::SliderFloat("Sharpness", &NISSettings.Sharpness, 0, 1, "%.2f", ImGuiSliderFlags_AlwaysClamp);
							}
						}
					}

					ImGui::Checkbox("Chromatic Aberration", &postProcessingSetttings.IsChromaticAberrationEnabled);

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

			if (ImGui::Button("Save")) ignore = MyAppData::Settings::Save();
		}
	}

	void RenderPopupModalWindow(string_view popupModalName) {
		const auto PopupModal = [&](LPCSTR name, const auto& lambda) {
			if (name == popupModalName) ImGui::OpenPopup(name);

			ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetWorkCenter(), ImGuiCond_Always, { 0.5f, 0.5f });
			ImGui::SetNextWindowSize({});

			if (ImGuiEx::PopupModal popupModal(name, nullptr, ImGuiWindowFlags_HorizontalScrollbar); popupModal) {
				lambda();

				ImGui::Separator();

				{
					constexpr auto Text = "OK";
					ImGuiEx::AlignForWidth(ImGui::CalcTextSize(Text).x);
					if (ImGui::Button(Text)) ImGui::CloseCurrentPopup();
					ImGui::SetItemDefaultFocus();
				}
			}
		};

		PopupModal(
			"Controls",
			[] {
				const auto AddWidgets = [](LPCSTR treeLabel, LPCSTR tableID, const initializer_list<pair<LPCSTR, LPCSTR>>& list) {
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
