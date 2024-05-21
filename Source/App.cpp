module;

#include "directx/d3dx12.h"

#include <shellapi.h>

#include "pix.h"

#include "directxtk12/CommonStates.h"
#include "directxtk12/DescriptorHeap.h"
#include "directxtk12/DirectXHelpers.h"
#include "directxtk12/GamePad.h"
#include "directxtk12/Keyboard.h"
#include "directxtk12/Mouse.h"
#include "directxtk12/ResourceUploadBatch.h"
#include "directxtk12/SimpleMath.h"
#include "directxtk12/SpriteBatch.h"

#include "D3D12MemAlloc.h"

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
import CommonShaderData;
import DescriptorHeap;
import DeviceResources;
import ErrorHelpers;
import GPUBuffer;
import GPUMemoryAllocator;
import HaltonSamplePattern;
import LightPreparation;
import Material;
import Model;
import MyScene;
import PostProcessing.Bloom;
import PostProcessing.ChromaticAberration;
import PostProcessing.DenoisedComposition;
import Raytracing;
import RTXDI;
import RTXDIResources;
import SharedData;
import StepTimer;
import StringConverters;
import Texture;
import ThreadHelpers;

using namespace D3D12MA;
using namespace DirectX;
using namespace DirectX::SimpleMath;
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

#define MAKE_NAME(name) static constexpr LPCSTR name = #name;
#define MAKE_NAMEW(name) static constexpr LPCWSTR name = L#name;

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

			m_deviceResources->SetWindow(windowModeHelper.hWnd, windowModeHelper.GetResolution());

			m_deviceResources->CreateDeviceResources();
			CreateDeviceDependentResources();

			m_deviceResources->CreateWindowSizeDependentResources();
			CreateWindowSizeDependentResources();

			m_deviceResources->EnableVSync(g_graphicsSettings.IsVSyncEnabled);
			m_deviceResources->RequestHDR(g_graphicsSettings.IsHDREnabled);
		}

		m_inputDevices.Mouse->SetWindow(windowModeHelper.hWnd);

		windowModeHelper.SetFullscreenResolutionHandledByWindow(false);

		LoadScene();
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
		ignore = m_streamline->NewFrame();

		if (const auto pFuture = m_futures.find(FutureNames::Scene);
			pFuture != cend(m_futures) && pFuture->second.wait_for(0s) == future_status::ready) {
			m_futures.erase(pFuture);
		}

		m_stepTimer.Tick([&] { Update(); });

		Render();

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

		m_renderTextures = {};

		m_alphaBlending.reset();

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

		m_renderDescriptorHeap.reset();
		m_resourceDescriptorHeap.reset();

		m_GPUMemoryAllocator.reset();
		m_graphicsMemory.reset();
	}

	void OnDeviceRestored() override {
		CreateDeviceDependentResources();

		CreateWindowSizeDependentResources();

		LoadScene();
	}

private:
	WindowModeHelper& m_windowModeHelper;

	unique_ptr<DeviceResources> m_deviceResources = make_unique<DeviceResources>(DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_UNKNOWN, 2, D3D_FEATURE_LEVEL_12_1, D3D12_RAYTRACING_TIER_1_1, DeviceResources::c_DisableGpuTimeout);

	StepTimer m_stepTimer;

	unique_ptr<GraphicsMemory> m_graphicsMemory;
	unique_ptr<GPUMemoryAllocator> m_GPUMemoryAllocator;

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

	bool m_isShaderExecutionReorderingSupported{};

	XMUINT2 m_renderSize{};

	HaltonSamplePattern m_haltonSamplePattern;

	struct FutureNames {
		MAKE_NAME(Scene);
	};
	map<string, future<void>, less<>> m_futures;

	exception_ptr m_exception;
	mutex m_exceptionMutex;

	struct ResourceDescriptorIndex {
		enum {
			InColor, OutColor,
			InFinalColor, OutFinalColor,
			InPreviousLinearDepth, OutPreviousLinearDepth,
			InLinearDepth, OutLinearDepth,
			InNormalizedDepth, OutNormalizedDepth,
			InMotionVectors, OutMotionVectors,
			InPreviousBaseColorMetalness, OutPreviousBaseColorMetalness,
			InBaseColorMetalness, OutBaseColorMetalness,
			InEmissiveColor, OutEmissiveColor,
			InPreviousNormals, OutPreviousNormals,
			InNormals, OutNormals,
			InPreviousRoughness, OutPreviousRoughness,
			InRoughness, OutRoughness,
			InNormalRoughness, OutNormalRoughness,
			OutNoisyDiffuse, OutNoisySpecular,
			InDenoisedDiffuse, OutDenoisedDiffuse,
			InDenoisedSpecular, OutDenoisedSpecular,
			InValidation, OutValidation,
			InRTXDINeighborOffsets,
			InFont,
			Reserve,
			Count = 1 << 16
		};
	};
	unique_ptr<DescriptorHeapEx> m_resourceDescriptorHeap;

	struct RenderDescriptorIndex {
		enum {
			Color,
			FinalColor,
			Count
		};
	};
	unique_ptr<DescriptorHeap> m_renderDescriptorHeap;

	unique_ptr<Raytracing> m_raytracing;

	RTXDIResources m_RTXDIResources;
	unique_ptr<LightPreparation> m_lightPreparation;
	unique_ptr<RTXDI> m_RTXDI;

	Constants m_slConstants = [] {
		Constants constants;
		constants.cameraPinholeOffset = { 0, 0 };
		constants.depthInverted = Boolean::eTrue;
		constants.cameraMotionIncluded = Boolean::eTrue;
		constants.motionVectors3D = Boolean::eFalse;
		constants.reset = Boolean::eFalse;
		return constants;
	}();
	unique_ptr<Streamline> m_streamline;

	bool m_isReflexLowLatencyAvailable{};

	DLSSGOptions m_DLSSGOptions;

	CommonSettings m_NRDCommonSettings{ .isBaseColorMetalnessAvailable = true };
	ReblurSettings m_NRDReblurSettings{ .hitDistanceReconstructionMode = HitDistanceReconstructionMode::AREA_3X3, .enableAntiFirefly = true };
	RelaxSettings m_NRDRelaxSettings{ .hitDistanceReconstructionMode = HitDistanceReconstructionMode::AREA_3X3, .enableAntiFirefly = true };
	unique_ptr<NRD> m_NRD;
	unique_ptr<DenoisedComposition> m_denoisedComposition;

	XeSSSettings m_XeSSSettings;
	unique_ptr<XeSS> m_XeSS;

	unique_ptr<ChromaticAberration> m_chromaticAberration;

	unique_ptr<Bloom> m_bloom;

	unique_ptr<ToneMapPostProcess> m_toneMapping[ToneMapPostProcess::Operator_Max + 1];

	unique_ptr<SpriteBatch> m_alphaBlending;

	struct RenderTextureNames {
		MAKE_NAMEW(Color);
		MAKE_NAMEW(FinalColor);
		MAKE_NAMEW(PreviousLinearDepth);
		MAKE_NAMEW(LinearDepth);
		MAKE_NAMEW(NormalizedDepth);
		MAKE_NAMEW(MotionVectors);
		MAKE_NAMEW(PreviousBaseColorMetalness);
		MAKE_NAMEW(BaseColorMetalness);
		MAKE_NAMEW(EmissiveColor);
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
	map<wstring, shared_ptr<Texture>, less<>> m_renderTextures;

	struct {
		shared_ptr<ConstantBuffer<Camera>> Camera;
		shared_ptr<ConstantBuffer<SceneData>> SceneData;
		shared_ptr<UploadBuffer<InstanceData>> InstanceData;
		shared_ptr<UploadBuffer<ObjectData>> ObjectData;
	} m_GPUBuffers;

	Camera m_camera;
	CameraController m_cameraController;

	shared_ptr<Scene> m_scene;

	struct { bool IsVisible, HasFocus = true, IsSettingsWindowOpen; } m_UIStates{};

	void CreateDeviceDependentResources() {
		const auto device = m_deviceResources->GetDevice();

		m_graphicsMemory = make_unique<GraphicsMemory>(device);

		m_GPUMemoryAllocator = make_unique<GPUMemoryAllocator>(ALLOCATOR_DESC{ .pDevice = device, .pAdapter = m_deviceResources->GetAdapter() });

		{
			NVAPI_D3D12_RAYTRACING_THREAD_REORDERING_CAPS caps;
			m_isShaderExecutionReorderingSupported = NvAPI_D3D12_GetRaytracingCaps(device, NVAPI_D3D12_RAYTRACING_CAPS_TYPE_THREAD_REORDERING, &caps, sizeof(caps)) == NVAPI_OK
				&& caps & NVAPI_D3D12_RAYTRACING_THREAD_REORDERING_CAP_STANDARD
				&& NvAPI_D3D12_SetNvShaderExtnSlotSpace(device, 1024, 0) == NVAPI_OK;
		}

		CreateDescriptorHeaps();

		CreatePipelineStates();

		CreateConstantBuffers();
	}

	void CreateWindowSizeDependentResources() {
		const auto device = m_deviceResources->GetDevice();
		const auto commandList = m_deviceResources->GetCommandList();

		const auto outputSize = GetOutputSize();

		{
			const auto CreateTexture = [&](LPCWSTR name, DXGI_FORMAT format, UINT SRVDescriptorIndex = ~0u, UINT UAVDescriptorIndex = ~0u, UINT RTVDescriptorIndex = ~0u) {
				auto& texture = m_renderTextures[name];
				texture = make_shared<Texture>(device, format, XMUINT2(outputSize.cx, outputSize.cy));
				texture->GetNative()->SetName(name);
				if (SRVDescriptorIndex != ~0u) texture->CreateSRV(*m_resourceDescriptorHeap, SRVDescriptorIndex);
				if (UAVDescriptorIndex != ~0u) texture->CreateUAV(*m_resourceDescriptorHeap, UAVDescriptorIndex);
				if (RTVDescriptorIndex != ~0u) texture->CreateRTV(*m_renderDescriptorHeap, RTVDescriptorIndex);
			};

			CreateTexture(RenderTextureNames::Color, DXGI_FORMAT_R16G16B16A16_FLOAT, ResourceDescriptorIndex::InColor, ResourceDescriptorIndex::OutColor, RenderDescriptorIndex::Color);
			CreateTexture(RenderTextureNames::FinalColor, DXGI_FORMAT_R16G16B16A16_FLOAT, ResourceDescriptorIndex::InFinalColor, ResourceDescriptorIndex::OutFinalColor, RenderDescriptorIndex::FinalColor);
			CreateTexture(RenderTextureNames::PreviousLinearDepth, DXGI_FORMAT_R32_FLOAT, ResourceDescriptorIndex::InPreviousLinearDepth, ResourceDescriptorIndex::OutPreviousLinearDepth);
			CreateTexture(RenderTextureNames::LinearDepth, DXGI_FORMAT_R32_FLOAT, ResourceDescriptorIndex::InLinearDepth, ResourceDescriptorIndex::OutLinearDepth);
			CreateTexture(RenderTextureNames::NormalizedDepth, DXGI_FORMAT_R32_FLOAT, ResourceDescriptorIndex::InNormalizedDepth, ResourceDescriptorIndex::OutNormalizedDepth);
			CreateTexture(RenderTextureNames::MotionVectors, DXGI_FORMAT_R16G16B16A16_FLOAT, ResourceDescriptorIndex::InMotionVectors, ResourceDescriptorIndex::OutMotionVectors);
			CreateTexture(RenderTextureNames::PreviousBaseColorMetalness, DXGI_FORMAT_R8G8B8A8_UNORM, ResourceDescriptorIndex::InPreviousBaseColorMetalness, ResourceDescriptorIndex::OutPreviousBaseColorMetalness);
			CreateTexture(RenderTextureNames::BaseColorMetalness, DXGI_FORMAT_R8G8B8A8_UNORM, ResourceDescriptorIndex::InBaseColorMetalness, ResourceDescriptorIndex::OutBaseColorMetalness);
			CreateTexture(RenderTextureNames::EmissiveColor, DXGI_FORMAT_R11G11B10_FLOAT, ResourceDescriptorIndex::InEmissiveColor, ResourceDescriptorIndex::OutEmissiveColor);
			CreateTexture(RenderTextureNames::PreviousNormals, DXGI_FORMAT_R16G16B16A16_SNORM, ResourceDescriptorIndex::InPreviousNormals, ResourceDescriptorIndex::OutPreviousNormals);
			CreateTexture(RenderTextureNames::Normals, DXGI_FORMAT_R16G16B16A16_SNORM, ResourceDescriptorIndex::InNormals, ResourceDescriptorIndex::OutNormals);
			CreateTexture(RenderTextureNames::PreviousRoughness, DXGI_FORMAT_R8_UNORM, ResourceDescriptorIndex::InPreviousRoughness, ResourceDescriptorIndex::OutPreviousRoughness);
			CreateTexture(RenderTextureNames::Roughness, DXGI_FORMAT_R8_UNORM, ResourceDescriptorIndex::InRoughness, ResourceDescriptorIndex::OutRoughness);
			CreateTexture(RenderTextureNames::NormalRoughness, NRD::ToDXGIFormat(GetLibraryDesc().normalEncoding), ResourceDescriptorIndex::InNormalRoughness, ResourceDescriptorIndex::OutNormalRoughness);

			if (m_NRD = make_unique<NRD>(
				device, m_deviceResources->GetCommandQueue(), commandList,
				static_cast<uint16_t>(outputSize.cx), static_cast<uint16_t>(outputSize.cy),
				m_deviceResources->GetBackBufferCount(),
				initializer_list<DenoiserDesc>{
					{ static_cast<Identifier>(NRDDenoiser::ReBLUR), Denoiser::REBLUR_DIFFUSE_SPECULAR },
					{ static_cast<Identifier>(NRDDenoiser::ReLAX), Denoiser::RELAX_DIFFUSE_SPECULAR }
			}
			);
				m_NRD->IsAvailable()) {
				CreateTexture(RenderTextureNames::NoisyDiffuse, DXGI_FORMAT_R16G16B16A16_FLOAT, ~0u, ResourceDescriptorIndex::OutNoisyDiffuse);
				CreateTexture(RenderTextureNames::NoisySpecular, DXGI_FORMAT_R16G16B16A16_FLOAT, ~0u, ResourceDescriptorIndex::OutNoisySpecular);
				CreateTexture(RenderTextureNames::DenoisedDiffuse, DXGI_FORMAT_R16G16B16A16_FLOAT, ResourceDescriptorIndex::InDenoisedDiffuse, ResourceDescriptorIndex::OutDenoisedDiffuse);
				CreateTexture(RenderTextureNames::DenoisedSpecular, DXGI_FORMAT_R16G16B16A16_FLOAT, ResourceDescriptorIndex::InDenoisedSpecular, ResourceDescriptorIndex::OutDenoisedSpecular);
				CreateTexture(RenderTextureNames::Validation, DXGI_FORMAT_R8G8B8A8_UNORM, ResourceDescriptorIndex::InValidation, ResourceDescriptorIndex::OutValidation);
			}

			m_XeSS = make_unique<XeSS>(device, xess_2d_t{ static_cast<uint32_t>(outputSize.cx), static_cast<uint32_t>(outputSize.cy) }, XESS_INIT_FLAG_INVERTED_DEPTH | XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE);

			SelectSuperResolutionUpscaler();
			SetSuperResolutionOptions();
		}

		m_cameraController.SetLens(XMConvertToRadians(g_graphicsSettings.Camera.HorizontalFieldOfView), static_cast<float>(outputSize.cx) / static_cast<float>(outputSize.cy));

		{
			const auto& IO = ImGui::GetIO();

			if (IO.BackendRendererUserData != nullptr) ImGui_ImplDX12_Shutdown();
			ImGui_ImplDX12_Init(device, m_deviceResources->GetBackBufferCount(), m_deviceResources->GetBackBufferFormat(), m_resourceDescriptorHeap->Heap(), m_resourceDescriptorHeap->GetCpuHandle(ResourceDescriptorIndex::InFont), m_resourceDescriptorHeap->GetGpuHandle(ResourceDescriptorIndex::InFont));

			IO.Fonts->Clear();
			IO.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/segoeui.ttf", static_cast<float>(outputSize.cy) * 0.022f);
		}
	}

	void Update() {
		const auto isPCLAvailable = m_streamline->IsFeatureAvailable(kFeaturePCL);

		if (isPCLAvailable) ignore = m_streamline->SetPCLMarker(PCLMarker::eSimulationStart);

		{
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

			m_GPUBuffers.Camera->At(0) = m_camera;
		}

		if (IsSceneReady()) UpdateScene();

		if (isPCLAvailable) ignore = m_streamline->SetPCLMarker(PCLMarker::eSimulationEnd);
	}

	void Render() {
		if (!m_stepTimer.GetFrameCount()) return;

		m_deviceResources->Prepare();

		const auto commandList = m_deviceResources->GetCommandList();

		const auto isPCLAvailable = m_streamline->IsFeatureAvailable(kFeaturePCL);

		if (isPCLAvailable) ignore = m_streamline->SetPCLMarker(PCLMarker::eRenderSubmitStart);

		const auto renderTargetView = m_deviceResources->GetRenderTargetView();
		commandList->OMSetRenderTargets(1, &renderTargetView, FALSE, nullptr);
		commandList->ClearRenderTargetView(renderTargetView, Colors::Black, 0, nullptr);

		const auto viewport = m_deviceResources->GetScreenViewport();
		const auto scissorRect = m_deviceResources->GetScissorRect();
		commandList->RSSetViewports(1, &viewport);
		commandList->RSSetScissorRects(1, &scissorRect);

		{
			const ScopedPixEvent scopedPixEvent(commandList, PIX_COLOR_DEFAULT, L"Render");

			const auto descriptorHeap = m_resourceDescriptorHeap->Heap();
			commandList->SetDescriptorHeaps(1, &descriptorHeap);

			if (IsSceneReady()) {
				if (!m_scene->IsStatic()) m_scene->CreateAccelerationStructures(true);

				RenderScene();

				PostProcessGraphics();
			}

			if (m_UIStates.IsVisible) RenderUI();
		}

		if (isPCLAvailable) {
			ignore = m_streamline->SetPCLMarker(PCLMarker::eRenderSubmitEnd);

			ignore = m_streamline->SetPCLMarker(PCLMarker::ePresentStart);
		}

		m_deviceResources->Present();

		if (isPCLAvailable) ignore = m_streamline->SetPCLMarker(PCLMarker::ePresentEnd);

		m_deviceResources->WaitForGpu();

		m_graphicsMemory->Commit(m_deviceResources->GetCommandQueue());

		{
			swap(m_renderTextures.at(RenderTextureNames::PreviousLinearDepth), m_renderTextures.at(RenderTextureNames::LinearDepth));
			swap(m_renderTextures.at(RenderTextureNames::PreviousBaseColorMetalness), m_renderTextures.at(RenderTextureNames::BaseColorMetalness));
			swap(m_renderTextures.at(RenderTextureNames::PreviousNormals), m_renderTextures.at(RenderTextureNames::Normals));
			swap(m_renderTextures.at(RenderTextureNames::PreviousRoughness), m_renderTextures.at(RenderTextureNames::Roughness));
		}
	}

	bool IsSceneLoading() const { return m_futures.contains(FutureNames::Scene); }
	bool IsSceneReady() const { return !IsSceneLoading() && m_scene; }

	void LoadScene() {
		m_futures[FutureNames::Scene] = StartDetachedFuture([&] {
			try {
			UINT descriptorIndex = ResourceDescriptorIndex::Reserve;
			m_scene = make_shared<MyScene>(m_deviceResources->GetDevice(), m_deviceResources->GetCommandQueue());
			m_scene->Load(MySceneDesc(), *m_resourceDescriptorHeap, descriptorIndex);

			CreateStructuredBuffers();

			ResetCamera();

			PrepareLightResources();
		}
		catch (...) {
			const scoped_lock lock(m_exceptionMutex);

			if (!m_exception) m_exception = current_exception();
		}
			});
	}

	void CreateDescriptorHeaps() {
		const auto device = m_deviceResources->GetDevice();

		m_resourceDescriptorHeap = make_unique<DescriptorHeapEx>(device, ResourceDescriptorIndex::Count, ResourceDescriptorIndex::Reserve);

		m_renderDescriptorHeap = make_unique<DescriptorHeap>(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, RenderDescriptorIndex::Count);
	}

	void CreatePipelineStates() {
		const auto device = m_deviceResources->GetDevice();

		m_raytracing = make_unique<Raytracing>(device);

		m_lightPreparation = make_unique<LightPreparation>(device);
		m_RTXDI = make_unique<RTXDI>(device);

		{
			ignore = slSetD3DDevice(device);

			DXGI_ADAPTER_DESC adapterDesc;
			ThrowIfFailed(m_deviceResources->GetAdapter()->GetDesc(&adapterDesc));
			m_streamline = make_unique<Streamline>(m_deviceResources->GetCommandList(), &adapterDesc.AdapterLuid, sizeof(adapterDesc.AdapterLuid), 0);

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
		const auto device = m_deviceResources->GetDevice();

		m_denoisedComposition = make_unique<DenoisedComposition>(device);

		m_chromaticAberration = make_unique<ChromaticAberration>(device);

		m_bloom = make_unique<Bloom>(device);

		{
			const RenderTargetState renderTargetState(m_deviceResources->GetBackBufferFormat(), DXGI_FORMAT_UNKNOWN);

			for (const auto Operator : { ToneMapPostProcess::None, ToneMapPostProcess::Saturate, ToneMapPostProcess::Reinhard, ToneMapPostProcess::ACESFilmic, ToneMapPostProcess::Operator_Max }) {
				m_toneMapping[Operator] = make_unique<ToneMapPostProcess>(
					device,
					renderTargetState,
					Operator == ToneMapPostProcess::Operator_Max ? ToneMapPostProcess::None : Operator,
					Operator == ToneMapPostProcess::Operator_Max ? ToneMapPostProcess::ST2084 :
					Operator == ToneMapPostProcess::None ? ToneMapPostProcess::Linear : ToneMapPostProcess::SRGB
					);
			}

			{
				ResourceUploadBatch resourceUploadBatch(device);
				resourceUploadBatch.Begin();

				m_alphaBlending = make_unique<SpriteBatch>(device, resourceUploadBatch, SpriteBatchPipelineStateDescription(renderTargetState, &CommonStates::NonPremultiplied));

				resourceUploadBatch.End(m_deviceResources->GetCommandQueue()).get();
			}
		}
	}

	void CreateConstantBuffers() {
		const auto CreateBuffer = [&]<typename T>(shared_ptr<T>&buffer, const auto & data) { buffer = make_shared<T>(m_deviceResources->GetDevice(), initializer_list{ data }); };
		CreateBuffer(m_GPUBuffers.Camera, Camera{ .IsNormalizedDepthReversed = true });
		CreateBuffer(m_GPUBuffers.SceneData, SceneData());
	}

	void CreateStructuredBuffers() {
		const auto CreateBuffer = [&]<typename T>(shared_ptr<T>&buffer, size_t capacity) { buffer = make_shared<T>(m_deviceResources->GetDevice(), capacity); };
		if (const auto instanceCount = size(m_scene->GetInstanceData())) CreateBuffer(m_GPUBuffers.InstanceData, instanceCount);
		if (const auto objectCount = m_scene->GetObjectCount()) CreateBuffer(m_GPUBuffers.ObjectData, objectCount);
	}

	void PrepareLightResources() {
		const auto device = m_deviceResources->GetDevice();

		m_lightPreparation->SetScene(m_scene.get());
		if (const auto emissiveTriangleCount = m_lightPreparation->GetEmissiveTriangleCount()) {
			m_RTXDIResources.CreateLightBuffers(device, emissiveTriangleCount, m_scene->GetObjectCount());

			{
				ResourceUploadBatch resourceUploadBatch(device);
				resourceUploadBatch.Begin();

				m_lightPreparation->PrepareResources(resourceUploadBatch, *m_RTXDIResources.LightIndices);

				resourceUploadBatch.End(m_deviceResources->GetCommandQueue()).get();
			}
		}
	}

	void PrepareLights() {
		m_lightPreparation->GPUBuffers = {
			.InInstanceData = m_GPUBuffers.InstanceData.get(),
			.InObjectData = m_GPUBuffers.ObjectData.get(),
			.OutLightInfo = m_RTXDIResources.LightInfo.get()
		};

		m_lightPreparation->Process(m_deviceResources->GetCommandList());
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

		ResetTemporalAccumulation();
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

		{
			auto& sceneData = m_GPUBuffers.SceneData->At(0);

			sceneData.IsStatic = m_scene->IsStatic();

			sceneData.ResourceDescriptorIndices = {};

			const auto IsCubeMap = [](ID3D12Resource* pResource) {
				if (pResource == nullptr) return false;
				const auto desc = pResource->GetDesc();
				return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.DepthOrArraySize == 6;
			};
			if (m_scene->EnvironmentLightTexture.Texture) {
				sceneData.IsEnvironmentLightTextureCubeMap = IsCubeMap(*m_scene->EnvironmentLightTexture.Texture);
				sceneData.ResourceDescriptorIndices.InEnvironmentLightTexture = m_scene->EnvironmentLightTexture.Texture->GetSRVDescriptor().Index;
				XMStoreFloat3x4(&sceneData.EnvironmentLightTextureTransform, m_scene->EnvironmentLightTexture.Transform());
			}
			if (m_scene->EnvironmentTexture.Texture) {
				sceneData.IsEnvironmentTextureCubeMap = IsCubeMap(*m_scene->EnvironmentTexture.Texture);
				sceneData.ResourceDescriptorIndices.InEnvironmentTexture = m_scene->EnvironmentTexture.Texture->GetSRVDescriptor().Index;
				XMStoreFloat3x4(&sceneData.EnvironmentTextureTransform, m_scene->EnvironmentTexture.Transform());
			}

			sceneData.EnvironmentLightColor = m_scene->EnvironmentLightColor;
			sceneData.EnvironmentColor = m_scene->EnvironmentColor;
		}

		for (UINT instanceIndex = 0; const auto & renderObject : m_scene->RenderObjects) {
			const auto& instanceData = m_scene->GetInstanceData()[instanceIndex];
			m_GPUBuffers.InstanceData->At(instanceIndex) = {
				.FirstGeometryIndex = instanceData.FirstGeometryIndex,
				.PreviousObjectToWorld = instanceData.PreviousObjectToWorld,
				.ObjectToWorld = instanceData.ObjectToWorld
			};
			instanceIndex++;

			const auto& mesh = renderObject.Mesh;

			auto& objectData = m_GPUBuffers.ObjectData->At(instanceData.FirstGeometryIndex);

			objectData.VertexDesc = mesh->GetVertexDesc();

			objectData.Material = renderObject.Material;

			auto& resourceDescriptorIndices = objectData.ResourceDescriptorIndices;
			resourceDescriptorIndices = {
				.Mesh{
					.Vertices = mesh->Vertices->GetRawSRVDescriptor().Index,
					.Indices = mesh->Indices->GetStructuredSRVDescriptor().Index
				}
			};

			for (const auto& [TextureType, Texture] : renderObject.Textures) {
				const auto index = Texture->GetSRVDescriptor().Index;
				switch (auto& indices = resourceDescriptorIndices.Textures; TextureType) {
					case TextureMap::BaseColor: indices.BaseColorMap = index; break;
					case TextureMap::EmissiveColor: indices.EmissiveColorMap = index; break;
					case TextureMap::Metallic: indices.MetallicMap = index; break;
					case TextureMap::Roughness: indices.RoughnessMap = index; break;
					case TextureMap::AmbientOcclusion: indices.AmbientOcclusionMap = index; break;
					case TextureMap::Transmission: indices.TransmissionMap = index; break;
					case TextureMap::Opacity: indices.OpacityMap = index; break;
					case TextureMap::Normal: indices.NormalMap = index; break;
					default: Throw<out_of_range>("Unsupported texture type");
				}
			}
		}
	}

	void RenderScene() {
		const auto commandList = m_deviceResources->GetCommandList();

		const auto frameIndex = m_stepTimer.GetFrameCount() - 1;

		const NRDSettings NRDSettings{
			.Denoiser = m_NRD->IsAvailable() ? g_graphicsSettings.PostProcessing.NRD.Denoiser : NRDDenoiser::None,
			.HitDistanceParameters = reinterpret_cast<const XMFLOAT4&>(m_NRDReblurSettings.hitDistanceParameters)
		};

		const auto
			color = m_renderTextures.at(RenderTextureNames::Color).get(),
			linearDepth = m_renderTextures.at(RenderTextureNames::LinearDepth).get(),
			motionVectors = m_renderTextures.at(RenderTextureNames::MotionVectors).get(),
			baseColorMetalness = m_renderTextures.at(RenderTextureNames::BaseColorMetalness).get(),
			normals = m_renderTextures.at(RenderTextureNames::Normals).get(),
			roughness = m_renderTextures.at(RenderTextureNames::Roughness).get(),
			noisyDiffuse = m_renderTextures.at(RenderTextureNames::NoisyDiffuse).get(),
			noisySpecular = m_renderTextures.at(RenderTextureNames::NoisySpecular).get();

		const auto& scene = m_scene->GetTopLevelAccelerationStructure();

		{
			const auto& raytracingSettings = g_graphicsSettings.Raytracing;
			m_raytracing->SetConstants({
				.RenderSize = m_renderSize,
				.FrameIndex = frameIndex,
				.Bounces = raytracingSettings.Bounces,
				.SamplesPerPixel = raytracingSettings.SamplesPerPixel,
				.IsRussianRouletteEnabled = raytracingSettings.IsRussianRouletteEnabled,
				.IsShaderExecutionReorderingEnabled = IsShaderExecutionReorderingEnabled(),
				.NRD = NRDSettings
				});

			m_raytracing->GPUBuffers = {
				.InSceneData = m_GPUBuffers.SceneData.get(),
				.InCamera = m_GPUBuffers.Camera.get(),
				.InInstanceData = m_GPUBuffers.InstanceData.get(),
				.InObjectData = m_GPUBuffers.ObjectData.get()
			};

			m_raytracing->RenderTextures = {
				.OutColor = color,
				.OutLinearDepth = linearDepth,
				.OutNormalizedDepth = m_renderTextures.at(RenderTextureNames::NormalizedDepth).get(),
				.OutMotionVectors = motionVectors,
				.OutBaseColorMetalness = baseColorMetalness,
				.OutEmissiveColor = m_renderTextures.at(RenderTextureNames::EmissiveColor).get(),
				.OutNormals = normals,
				.OutRoughness = roughness,
				.OutNormalRoughness = m_renderTextures.at(RenderTextureNames::NormalRoughness).get(),
				.OutNoisyDiffuse = noisyDiffuse,
				.OutNoisySpecular = noisySpecular
			};

			m_raytracing->Render(commandList, scene);
		}

		if (const auto& lightBufferParameters = m_lightPreparation->GetLightBufferParameters();
			g_graphicsSettings.Raytracing.IsRTXDIEnabled
			&& (lightBufferParameters.localLightBufferRegion.numLights
				|| lightBufferParameters.infiniteLightBufferRegion.numLights
				|| lightBufferParameters.environmentLightParams.lightPresent)) {
			PrepareLights();

			auto& ReSTIRDIContext = m_RTXDIResources.Context->getReSTIRDIContext();
			ReSTIRDIContext.setFrameIndex(frameIndex);
			m_RTXDI->SetConstants({
				.RenderSize = m_renderSize,
				.FrameIndex = frameIndex,
				.RTXDI{
					.LightBuffer = lightBufferParameters,
					.Runtime = ReSTIRDIContext.getRuntimeParams(),
					.ReSTIRDI{
						.reservoirBufferParams = ReSTIRDIContext.getReservoirBufferParameters(),
						.bufferIndices = ReSTIRDIContext.getBufferIndices(),
						.initialSamplingParams = ReSTIRDIContext.getInitialSamplingParameters(),
						.temporalResamplingParams = ReSTIRDIContext.getTemporalResamplingParameters(),
						.spatialResamplingParams = ReSTIRDIContext.getSpatialResamplingParameters(),
						.shadingParams = ReSTIRDIContext.getShadingParameters()
					}
				},
				.NRD = NRDSettings
				});

			m_RTXDI->GPUBuffers = {
				.InCamera = m_GPUBuffers.Camera.get(),
				.InInstanceData = m_GPUBuffers.InstanceData.get(),
				.InObjectData = m_GPUBuffers.ObjectData.get(),
				.InLightInfo = m_RTXDIResources.LightInfo.get(),
				.InLightIndices = m_RTXDIResources.LightIndices.get(),
				.InNeighborOffsets = m_RTXDIResources.NeighborOffsets.get(),
				.OutDIReservoir = m_RTXDIResources.DIReservoir.get()
			};

			m_RTXDI->RenderTextures = {
				.InPreviousLinearDepth = m_renderTextures.at(RenderTextureNames::PreviousLinearDepth).get(),
				.InLinearDepth = linearDepth,
				.InMotionVectors = motionVectors,
				.InPreviousBaseColorMetalness = m_renderTextures.at(RenderTextureNames::PreviousBaseColorMetalness).get(),
				.InBaseColorMetalness = baseColorMetalness,
				.InPreviousNormals = m_renderTextures.at(RenderTextureNames::PreviousNormals).get(),
				.InNormals = normals,
				.InPreviousRoughness = m_renderTextures.at(RenderTextureNames::PreviousRoughness).get(),
				.InRoughness = roughness,
				.OutColor = color,
				.OutNoisyDiffuse = noisyDiffuse,
				.OutNoisySpecular = noisySpecular
			};

			m_RTXDI->Render(commandList, scene);
		}
	}

	bool IsDLSSFrameGenerationEnabled() const { return g_graphicsSettings.PostProcessing.IsDLSSFrameGenerationEnabled && m_streamline->IsFeatureAvailable(kFeatureDLSS_G) && IsReflexEnabled(); }
	bool IsDLSSSuperResolutionEnabled() const { return g_graphicsSettings.PostProcessing.SuperResolution.Upscaler == Upscaler::DLSS && m_streamline->IsFeatureAvailable(kFeatureDLSS); }
	bool IsNISEnabled() const { return g_graphicsSettings.PostProcessing.NIS.IsEnabled && m_streamline->IsFeatureAvailable(kFeatureNIS); }
	bool IsNRDEnabled() const { return g_graphicsSettings.PostProcessing.NRD.Denoiser != NRDDenoiser::None && m_NRD->IsAvailable(); }
	bool IsReflexEnabled() const { return g_graphicsSettings.ReflexMode != ReflexMode::eOff && m_streamline->IsFeatureAvailable(kFeatureReflex); }
	bool IsShaderExecutionReorderingEnabled() const { return m_isShaderExecutionReorderingSupported && g_graphicsSettings.Raytracing.IsShaderExecutionReorderingEnabled; }
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
		const auto outputSize = m_deviceResources->GetOutputSize();

		const auto SelectSuperResolutionMode = [&] {
			if (g_graphicsSettings.PostProcessing.SuperResolution.Mode != SuperResolutionMode::Auto) return g_graphicsSettings.PostProcessing.SuperResolution.Mode;
			const auto minValue = min(outputSize.cx, outputSize.cy);
			if (minValue <= 720) return SuperResolutionMode::Native;
			if (minValue <= 1440) return SuperResolutionMode::Quality;
			if (minValue <= 2160) return SuperResolutionMode::Performance;
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

	void ResetTemporalAccumulation() {
		m_slConstants.reset = Boolean::eTrue;

		m_NRDCommonSettings.accumulationMode = AccumulationMode::CLEAR_AND_RESTART;

		m_XeSSSettings.Reset = true;
	}

	void OnRenderSizeChanged() {
		const auto outputSize = GetOutputSize();
		m_haltonSamplePattern = HaltonSamplePattern(static_cast<UINT>(8 * (static_cast<float>(outputSize.cx) / static_cast<float>(m_renderSize.x)) * (static_cast<float>(outputSize.cy) / static_cast<float>(m_renderSize.y))));

		ResetTemporalAccumulation();

		{
			m_RTXDIResources.Context = make_unique<ImportanceSamplingContext>(ImportanceSamplingContext_StaticParameters{ .renderWidth = m_renderSize.x, .renderHeight = m_renderSize.y });

			auto& ReSTIRDIContext = m_RTXDIResources.Context->getReSTIRDIContext();
			auto temporalResamplingParameters = m_RTXDIResources.Context->getReSTIRDIContext().getTemporalResamplingParameters();
			temporalResamplingParameters.temporalBiasCorrection = ReSTIRDI_TemporalBiasCorrectionMode::Raytraced;
			ReSTIRDIContext.setTemporalResamplingParameters(temporalResamplingParameters);

			const auto device = m_deviceResources->GetDevice();

			ResourceUploadBatch resourceUploadBatch(device);
			resourceUploadBatch.Begin();

			m_RTXDIResources.CreateNeighborOffsets(device, resourceUploadBatch, *m_resourceDescriptorHeap, ResourceDescriptorIndex::InRTXDINeighborOffsets);

			resourceUploadBatch.End(m_deviceResources->GetCommandQueue()).get();

			m_RTXDIResources.CreateDIReservoir(device);
		}
	}

	void PostProcessGraphics() {
		const auto& postProcessingSettings = g_graphicsSettings.PostProcessing;

		PrepareStreamline();

		const auto isNRDEnabled = IsNRDEnabled();

		if (isNRDEnabled) ProcessNRD();

		auto inColor = m_renderTextures.at(RenderTextureNames::Color).get(), outColor = m_renderTextures.at(RenderTextureNames::FinalColor).get();

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

		if (IsDLSSFrameGenerationEnabled()) ProcessDLSSFrameGeneration(*inColor);

		ProcessToneMapping(*inColor);

		if (isNRDEnabled && postProcessingSettings.NRD.IsValidationOverlayEnabled) ProcessAlphaBlending(*m_renderTextures.at(RenderTextureNames::Validation));
	}

	void PrepareStreamline() {
		reinterpret_cast<XMFLOAT4X4&>(m_slConstants.cameraViewToClip) = m_cameraController.GetViewToProjection();
		recalculateCameraMatrices(m_slConstants, reinterpret_cast<const float4x4&>(m_camera.PreviousViewToWorld), reinterpret_cast<const float4x4&>(m_camera.PreviousViewToProjection));
		m_slConstants.jitterOffset = { -m_camera.Jitter.x, -m_camera.Jitter.y };
		reinterpret_cast<XMFLOAT3&>(m_slConstants.cameraPos) = m_cameraController.GetPosition();
		reinterpret_cast<XMFLOAT3&>(m_slConstants.cameraUp) = m_cameraController.GetUpDirection();
		reinterpret_cast<XMFLOAT3&>(m_slConstants.cameraRight) = m_cameraController.GetRightDirection();
		reinterpret_cast<XMFLOAT3&>(m_slConstants.cameraFwd) = m_cameraController.GetForwardDirection();
		m_slConstants.cameraNear = m_cameraController.GetNearDepth();
		m_slConstants.cameraFar = m_cameraController.GetFarDepth();
		m_slConstants.cameraFOV = m_cameraController.GetVerticalFieldOfView();
		m_slConstants.cameraAspectRatio = m_cameraController.GetAspectRatio();

		m_slConstants.mvecScale = { 1 / static_cast<float>(m_renderSize.x), 1 / static_cast<float>(m_renderSize.y) };

		ignore = m_streamline->SetConstants(m_slConstants);

		m_slConstants.reset = Boolean::eFalse;
	}

	void ProcessNRD() {
		const auto commandList = m_deviceResources->GetCommandList();

		const auto& NRDSettings = g_graphicsSettings.PostProcessing.NRD;

		auto
			& linearDepth = *m_renderTextures.at(RenderTextureNames::LinearDepth),
			& baseColorMetalness = *m_renderTextures.at(RenderTextureNames::BaseColorMetalness),
			& normalRoughness = *m_renderTextures.at(RenderTextureNames::NormalRoughness),
			& denoisedDiffuse = *m_renderTextures.at(RenderTextureNames::DenoisedDiffuse),
			& denoisedSpecular = *m_renderTextures.at(RenderTextureNames::DenoisedSpecular);

		{
			m_NRD->NewFrame();

			const auto Tag = [&](nrd::ResourceType resourceType, const Texture& texture) { m_NRD->Tag(resourceType, texture, texture.GetState()); };
			Tag(nrd::ResourceType::IN_VIEWZ, linearDepth);
			Tag(nrd::ResourceType::IN_MV, *m_renderTextures.at(RenderTextureNames::MotionVectors));
			Tag(nrd::ResourceType::IN_BASECOLOR_METALNESS, baseColorMetalness);
			Tag(nrd::ResourceType::IN_NORMAL_ROUGHNESS, normalRoughness);
			Tag(nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST, *m_renderTextures.at(RenderTextureNames::NoisyDiffuse));
			Tag(nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST, *m_renderTextures.at(RenderTextureNames::NoisySpecular));
			Tag(nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST, denoisedDiffuse);
			Tag(nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST, denoisedSpecular);
			Tag(nrd::ResourceType::OUT_VALIDATION, *m_renderTextures.at(RenderTextureNames::Validation));

			reinterpret_cast<XMFLOAT4X4&>(m_NRDCommonSettings.worldToViewMatrixPrev) = m_camera.PreviousWorldToView;
			reinterpret_cast<XMFLOAT4X4&>(m_NRDCommonSettings.viewToClipMatrixPrev) = m_camera.PreviousViewToProjection;
			reinterpret_cast<XMFLOAT4X4&>(m_NRDCommonSettings.worldToViewMatrix) = m_cameraController.GetWorldToView();
			reinterpret_cast<XMFLOAT4X4&>(m_NRDCommonSettings.viewToClipMatrix) = m_cameraController.GetViewToProjection();
			ranges::copy(m_NRDCommonSettings.cameraJitter, m_NRDCommonSettings.cameraJitterPrev);
			reinterpret_cast<XMFLOAT2&>(m_NRDCommonSettings.cameraJitter) = m_camera.Jitter;

			const auto outputSize = GetOutputSize();
			m_NRDCommonSettings.resourceSize[0] = static_cast<uint16_t>(outputSize.cx);
			m_NRDCommonSettings.resourceSize[1] = static_cast<uint16_t>(outputSize.cy);
			ranges::copy(m_NRDCommonSettings.resourceSize, m_NRDCommonSettings.resourceSizePrev);

			m_NRDCommonSettings.rectSize[0] = static_cast<uint16_t>(m_renderSize.x);
			m_NRDCommonSettings.rectSize[1] = static_cast<uint16_t>(m_renderSize.y);
			ranges::copy(m_NRDCommonSettings.rectSize, m_NRDCommonSettings.rectSizePrev);

			reinterpret_cast<XMFLOAT3&>(m_NRDCommonSettings.motionVectorScale) = { 1 / static_cast<float>(m_renderSize.x), 1 / static_cast<float>(m_renderSize.y), 1 };

			m_NRDCommonSettings.enableValidation = NRDSettings.IsValidationOverlayEnabled;

			ignore = m_NRD->SetConstants(m_NRDCommonSettings);

			const auto denoiser = static_cast<Identifier>(NRDSettings.Denoiser);
			if (NRDSettings.Denoiser == NRDDenoiser::ReBLUR) ignore = m_NRD->SetConstants(denoiser, m_NRDReblurSettings);
			else if (NRDSettings.Denoiser == NRDDenoiser::ReLAX) ignore = m_NRD->SetConstants(denoiser, m_NRDRelaxSettings);
			m_NRD->Denoise(initializer_list<Identifier>{ denoiser });

			m_NRDCommonSettings.accumulationMode = AccumulationMode::CONTINUE;
		}

		const auto descriptorHeap = m_resourceDescriptorHeap->Heap();
		commandList->SetDescriptorHeaps(1, &descriptorHeap);

		{
			m_denoisedComposition->Constants = {
				.RenderSize = m_renderSize,
				.NRDDenoiser = NRDSettings.Denoiser
			};

			m_denoisedComposition->GPUBuffers = { .InCamera = m_GPUBuffers.Camera.get() };

			m_denoisedComposition->RenderTextures = {
				.InLinearDepth = &linearDepth,
				.InBaseColorMetalness = &baseColorMetalness,
				.InEmissiveColor = m_renderTextures.at(RenderTextureNames::EmissiveColor).get(),
				.InNormalRoughness = &normalRoughness,
				.InDenoisedDiffuse = &denoisedDiffuse,
				.InDenoisedSpecular = &denoisedSpecular,
				.OutColor = m_renderTextures.at(RenderTextureNames::Color).get()
			};

			m_denoisedComposition->Process(commandList);
		}
	}

	void ProcessDLSSSuperResolution() {
		ResourceTagDesc resourceTagDescs[]{
			CreateResourceTagDesc(kBufferTypeDepth, *m_renderTextures.at(RenderTextureNames::NormalizedDepth)),
			CreateResourceTagDesc(kBufferTypeMotionVectors, *m_renderTextures.at(RenderTextureNames::MotionVectors)),
			CreateResourceTagDesc(kBufferTypeScalingInputColor, *m_renderTextures.at(RenderTextureNames::Color)),
			CreateResourceTagDesc(kBufferTypeScalingOutputColor, *m_renderTextures.at(RenderTextureNames::FinalColor), false)
		};
		ignore = m_streamline->EvaluateFeature(kFeatureDLSS, CreateResourceTags(resourceTagDescs));

		const auto descriptorHeap = m_resourceDescriptorHeap->Heap();
		m_deviceResources->GetCommandList()->SetDescriptorHeaps(1, &descriptorHeap);
	}

	void ProcessDLSSFrameGeneration(Texture& inColor) {
		m_DLSSGOptions.mode = g_graphicsSettings.PostProcessing.IsDLSSFrameGenerationEnabled && g_graphicsSettings.ReflexMode != ReflexMode::eOff ? DLSSGMode::eAuto : DLSSGMode::eOff;
		ignore = m_streamline->SetConstants(m_DLSSGOptions);

		ResourceTagDesc resourceTagDescs[]{
			CreateResourceTagDesc(kBufferTypeDepth, *m_renderTextures.at(RenderTextureNames::NormalizedDepth)),
			CreateResourceTagDesc(kBufferTypeMotionVectors, *m_renderTextures.at(RenderTextureNames::MotionVectors)),
			CreateResourceTagDesc(kBufferTypeHUDLessColor, inColor, false)
		};
		ignore = m_streamline->Tag(CreateResourceTags(resourceTagDescs));
	}

	void ProcessXeSSSuperResolution() {
		const auto commandList = m_deviceResources->GetCommandList();

		reinterpret_cast<XMUINT2&>(m_XeSSSettings.InputSize) = m_renderSize;

		reinterpret_cast<XMFLOAT2&>(m_XeSSSettings.Jitter) = { -m_camera.Jitter.x, -m_camera.Jitter.y };

		m_XeSS->SetConstants(m_XeSSSettings);

		m_XeSSSettings.Reset = false;

		struct XeSSResource {
			XeSSResourceType Type;
			Texture& Texture;
			D3D12_RESOURCE_STATES State = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
		} resources[]{
			{ XeSSResourceType::Depth, *m_renderTextures.at(RenderTextureNames::NormalizedDepth) },
			{ XeSSResourceType::Velocity, *m_renderTextures.at(RenderTextureNames::MotionVectors) },
			{ XeSSResourceType::Color, *m_renderTextures.at(RenderTextureNames::Color) },
			{ XeSSResourceType::Output, *m_renderTextures.at(RenderTextureNames::FinalColor), D3D12_RESOURCE_STATE_UNORDERED_ACCESS }
		};

		for (auto& [Type, Texture, State] : resources) {
			const auto state = Texture.GetState();
			Texture.TransitionTo(commandList, State);
			m_XeSS->Tag(Type, Texture);
			State = state;
		}

		ignore = m_XeSS->Execute(commandList);

		for (const auto& [_, Texture, State] : resources) Texture.TransitionTo(commandList, State);

		const auto descriptorHeap = m_resourceDescriptorHeap->Heap();
		commandList->SetDescriptorHeaps(1, &descriptorHeap);
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

		const auto descriptorHeap = m_resourceDescriptorHeap->Heap();
		m_deviceResources->GetCommandList()->SetDescriptorHeaps(1, &descriptorHeap);
	}

	void ProcessChromaticAberration(Texture& inColor, Texture& outColor) {
		m_chromaticAberration->RenderTextures = {
			.Input = &inColor,
			.Output = &outColor
		};

		m_chromaticAberration->Process(m_deviceResources->GetCommandList());
	}

	void ProcessBloom(Texture& inColor, Texture& outColor) {
		const auto commandList = m_deviceResources->GetCommandList();

		m_bloom->Constants.Strength = g_graphicsSettings.PostProcessing.Bloom.Strength;

		m_bloom->SetTextures(inColor, outColor);

		m_bloom->Process(commandList);

		const auto descriptorHeap = m_resourceDescriptorHeap->Heap();
		commandList->SetDescriptorHeaps(1, &descriptorHeap);
	}

	void ProcessToneMapping(Texture& inColor) {
		const auto commandList = m_deviceResources->GetCommandList();

		const ScopedBarrier scopedBarrier(commandList, { CD3DX12_RESOURCE_BARRIER::Transition(inColor, inColor.GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE) });

		const auto isHDREnabled = m_deviceResources->IsHDREnabled();
		const auto toneMappingSettings = g_graphicsSettings.PostProcessing.ToneMapping;

		auto& toneMapping = *m_toneMapping[isHDREnabled ? ToneMapPostProcess::Operator_Max : toneMappingSettings.NonHDR.Operator];

		if (isHDREnabled) {
			toneMapping.SetST2084Parameter(toneMappingSettings.HDR.PaperWhiteNits);
			toneMapping.SetColorRotation(toneMappingSettings.HDR.ColorPrimaryRotation);
		}
		else toneMapping.SetExposure(toneMappingSettings.NonHDR.Exposure);

		toneMapping.SetHDRSourceTexture(inColor.GetSRVDescriptor().GPUHandle);

		toneMapping.Process(commandList);
	}

	void ProcessAlphaBlending(Texture& inColor) {
		const auto commandList = m_deviceResources->GetCommandList();

		const ScopedBarrier scopedBarrier(commandList, { CD3DX12_RESOURCE_BARRIER::Transition(inColor, inColor.GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE) });

		m_alphaBlending->SetViewport(m_deviceResources->GetScreenViewport());
		m_alphaBlending->Begin(commandList);
		m_alphaBlending->Draw(inColor.GetSRVDescriptor().GPUHandle, GetTextureSize(inColor), XMFLOAT2());
		m_alphaBlending->End();
	}

	void RenderUI() {
		ImGui_ImplDX12_NewFrame();
		ImGui_ImplWin32_NewFrame();

		const auto outputSize = GetOutputSize();
		ImGui::GetIO().DisplaySize = { static_cast<float>(outputSize.cx), static_cast<float>(outputSize.cy) };

		ImGui::NewFrame();

		const auto popupModalName = RenderMenuBar();

		if (m_UIStates.IsSettingsWindowOpen) RenderSettingsWindow();

		RenderPopupModalWindow(popupModalName);

		if (IsSceneLoading()) RenderLoadingSceneWindow();

		ImGui::Render();
		ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), m_deviceResources->GetCommandList());
	}

	string RenderMenuBar() {
		string popupModalName;
		if (ImGui::BeginMainMenuBar()) {
			if (ImGui::GetFrameCount() == 1) ImGui::SetKeyboardFocusHere();

			const auto PopupModal = [&](LPCSTR name) { if (ImGui::MenuItem(name)) popupModalName = name; };

			if (ImGui::BeginMenu("File")) {
				if (ImGui::MenuItem("Exit")) PostQuitMessage(ERROR_SUCCESS);

				ImGui::EndMenu();
			}

			if (ImGui::BeginMenu("View")) {
				m_UIStates.IsSettingsWindowOpen |= ImGui::MenuItem("Settings");

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
		return popupModalName;
	}

	void RenderSettingsWindow() {
		ImGui::SetNextWindowBgAlpha(g_UISettings.WindowOpacity);

		const auto& viewport = *ImGui::GetMainViewport();
		ImGui::SetNextWindowPos({ viewport.WorkPos.x, viewport.WorkPos.y });
		ImGui::SetNextWindowSize({});

		if (ImGui::Begin("Settings", &m_UIStates.IsSettingsWindowOpen, ImGuiWindowFlags_HorizontalScrollbar)) {
			if (ImGui::TreeNode("Graphics")) {
				{
					auto isChanged = false;

					if (ImGui::BeginCombo("Window Mode", ToString(g_graphicsSettings.WindowMode))) {
						for (const auto WindowMode : { WindowMode::Windowed, WindowMode::Borderless, WindowMode::Fullscreen }) {
							const auto isSelected = g_graphicsSettings.WindowMode == WindowMode;

							if (ImGui::Selectable(ToString(WindowMode), isSelected)) {
								g_graphicsSettings.WindowMode = WindowMode;

								m_windowModeHelper.SetMode(WindowMode);

								isChanged = true;
							}

							if (isSelected) ImGui::SetItemDefaultFocus();
						}

						ImGui::EndCombo();
					}

					if (const auto ToString = [](SIZE value) { return format("{} × {}", value.cx, value.cy); };
						ImGui::BeginCombo("Resolution", ToString(g_graphicsSettings.Resolution).c_str())) {
						for (const auto& resolution : g_displayResolutions) {
							const auto isSelected = g_graphicsSettings.Resolution == resolution;

							if (ImGui::Selectable(ToString(resolution).c_str(), isSelected)) {
								g_graphicsSettings.Resolution = resolution;

								m_windowModeHelper.SetResolution(resolution);

								isChanged = true;
							}

							if (isSelected) ImGui::SetItemDefaultFocus();
						}

						ImGui::EndCombo();
					}

					if (isChanged) m_futures["WindowSetting"] = async(launch::deferred, [&] { ThrowIfFailed(m_windowModeHelper.Apply()); });
				}

				{
					auto isEnabled = m_deviceResources->IsHDREnabled();
					if (const ImGuiEx::ScopedEnablement scopedEnablement(m_deviceResources->IsHDRSupported());
						ImGui::Checkbox("HDR", &isEnabled)) {
						g_graphicsSettings.IsHDREnabled = isEnabled;

						m_futures["HDRSetting"] = async(launch::deferred, [&] { m_deviceResources->RequestHDR(g_graphicsSettings.IsHDREnabled); });
					}
				}

				{
					auto isEnabled = m_deviceResources->IsVSyncEnabled();
					if (const ImGuiEx::ScopedEnablement scopedEnablement(m_deviceResources->IsTearingSupported());
						ImGui::Checkbox("V-Sync", &isEnabled)) {
						g_graphicsSettings.IsVSyncEnabled = isEnabled;

						m_deviceResources->EnableVSync(isEnabled);
					}
				}

				{
					if (const ImGuiEx::ScopedEnablement scopedEnablement(m_isReflexLowLatencyAvailable);
						ImGui::BeginCombo("NVIDIA Reflex", ToString(m_isReflexLowLatencyAvailable ? g_graphicsSettings.ReflexMode : ReflexMode::eOff))) {
						for (const auto ReflexMode : { ReflexMode::eOff, ReflexMode::eLowLatency, ReflexMode::eLowLatencyWithBoost }) {
							const auto isSelected = g_graphicsSettings.ReflexMode == ReflexMode;

							if (ImGui::Selectable(ToString(ReflexMode), isSelected)) {
								g_graphicsSettings.ReflexMode = ReflexMode;

								SetReflexOptions();
							}

							if (isSelected) ImGui::SetItemDefaultFocus();
						}

						ImGui::EndCombo();
					}
				}

				if (ImGui::TreeNodeEx("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
					auto& cameraSettings = g_graphicsSettings.Camera;

					if (ImGui::Checkbox("Jitter", &cameraSettings.IsJitterEnabled)) ResetTemporalAccumulation();

					if (ImGui::SliderFloat("Horizontal Field of View", &cameraSettings.HorizontalFieldOfView, cameraSettings.MinHorizontalFieldOfView, cameraSettings.MaxHorizontalFieldOfView, "%.1f°", ImGuiSliderFlags_AlwaysClamp)) {
						m_cameraController.SetLens(XMConvertToRadians(cameraSettings.HorizontalFieldOfView), m_cameraController.GetAspectRatio());
					}

					ImGui::TreePop();
				}

				if (ImGui::TreeNodeEx("Raytracing", ImGuiTreeNodeFlags_DefaultOpen)) {
					auto& raytracingSettings = g_graphicsSettings.Raytracing;

					auto isChanged = false;

					isChanged |= ImGui::Checkbox("Russian Roulette", &raytracingSettings.IsRussianRouletteEnabled);

					isChanged |= ImGui::SliderInt("Bounces", reinterpret_cast<int*>(&raytracingSettings.Bounces), 0, raytracingSettings.MaxBounces, "%d", ImGuiSliderFlags_AlwaysClamp);

					isChanged |= ImGui::SliderInt("Samples per Pixel", reinterpret_cast<int*>(&raytracingSettings.SamplesPerPixel), 1, raytracingSettings.MaxSamplesPerPixel, "%d", ImGuiSliderFlags_AlwaysClamp);

					{
						auto isEnabled = IsShaderExecutionReorderingEnabled();
						if (const ImGuiEx::ScopedEnablement scopedEnablement(m_isShaderExecutionReorderingSupported);
							ImGui::Checkbox("NVIDIA Shader Execution Reordering", &isEnabled)) {
							raytracingSettings.IsShaderExecutionReorderingEnabled = isEnabled;
						}
					}

					isChanged |= ImGui::Checkbox("NVIDIA RTX Dynamic Illumination", &raytracingSettings.IsRTXDIEnabled);

					if (isChanged) ResetTemporalAccumulation();

					ImGui::TreePop();
				}

				if (ImGui::TreeNodeEx("Post-Processing", ImGuiTreeNodeFlags_DefaultOpen)) {
					auto& postProcessingSetttings = g_graphicsSettings.PostProcessing;

					{
						const auto isAvailable = m_NRD->IsAvailable();
						if (const ImGuiEx::ScopedEnablement scopedEnablement(isAvailable);
							ImGui::TreeNodeEx("NVIDIA Real-Time Denoisers", isAvailable ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None)) {
							auto& NRDSettings = postProcessingSetttings.NRD;

							if (ImGui::BeginCombo("Denoiser", ToString(NRDSettings.Denoiser))) {
								for (const auto Denoiser : { NRDDenoiser::None, NRDDenoiser::ReBLUR, NRDDenoiser::ReLAX }) {
									const auto isSelected = NRDSettings.Denoiser == Denoiser;

									if (ImGui::Selectable(ToString(Denoiser), isSelected)) {
										NRDSettings.Denoiser = Denoiser;

										ResetTemporalAccumulation();
									}

									if (isSelected) ImGui::SetItemDefaultFocus();
								}

								ImGui::EndCombo();
							}

							if (NRDSettings.Denoiser != NRDDenoiser::None) ImGui::Checkbox("Validation Overlay", &NRDSettings.IsValidationOverlayEnabled);

							ImGui::TreePop();
						}
					}

					if (ImGui::TreeNodeEx("Super Resolution", ImGuiTreeNodeFlags_DefaultOpen)) {
						auto& superResolutionSettings = postProcessingSetttings.SuperResolution;

						auto isChanged = false;

						if (ImGui::BeginCombo("Upscaler", ToString(superResolutionSettings.Upscaler))) {
							for (const auto Upscaler : { Upscaler::None, Upscaler::DLSS, Upscaler::XeSS }) {
								const auto IsSelectable = [&] {
									return Upscaler == Upscaler::None
										|| (Upscaler == Upscaler::DLSS && m_streamline->IsFeatureAvailable(kFeatureDLSS))
										|| (Upscaler == Upscaler::XeSS && m_XeSS->IsAvailable());
								};
								const auto isSelected = superResolutionSettings.Upscaler == Upscaler;

								if (ImGui::Selectable(ToString(Upscaler), isSelected, IsSelectable() ? ImGuiSelectableFlags_None : ImGuiSelectableFlags_Disabled)) {
									superResolutionSettings.Upscaler = Upscaler;

									isChanged = true;
								}

								if (isSelected) ImGui::SetItemDefaultFocus();
							}

							ImGui::EndCombo();
						}

						if (superResolutionSettings.Upscaler != Upscaler::None && ImGui::BeginCombo("Mode", ToString(superResolutionSettings.Mode))) {
							for (const auto Mode : { SuperResolutionMode::Auto, SuperResolutionMode::Native, SuperResolutionMode::Quality, SuperResolutionMode::Balanced, SuperResolutionMode::Performance, SuperResolutionMode::UltraPerformance }) {
								const auto isSelected = superResolutionSettings.Mode == Mode;

								if (ImGui::Selectable(ToString(Mode), isSelected)) {
									superResolutionSettings.Mode = Mode;

									isChanged = true;
								}

								if (isSelected) ImGui::SetItemDefaultFocus();
							}

							ImGui::EndCombo();
						}

						if (isChanged) m_futures["SuperResolutionSetting"] = async(launch::deferred, [&] { SetSuperResolutionOptions(); });

						ImGui::TreePop();
					}

					if (IsReflexEnabled()) {
						auto isEnabled = IsDLSSFrameGenerationEnabled();
						if (const ImGuiEx::ScopedEnablement scopedEnablement(m_streamline->IsFeatureAvailable(kFeatureDLSS_G));
							ImGui::Checkbox("NVIDIA DLSS Frame Generation", &isEnabled)) {
							postProcessingSetttings.IsDLSSFrameGenerationEnabled = isEnabled;
						}
					}

					{
						const auto isAvailable = m_streamline->IsFeatureAvailable(kFeatureNIS);
						if (const ImGuiEx::ScopedEnablement scopedEnablement(isAvailable);
							ImGui::TreeNodeEx("NVIDIA Image Scaling", isAvailable ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None)) {
							auto& NISSettings = postProcessingSetttings.NIS;

							{
								const ImGuiEx::ScopedID scopedID("Enable NVIDIA Image Scaling");

								ImGui::Checkbox("Enable", &NISSettings.IsEnabled);
							}

							if (NISSettings.IsEnabled) ImGui::SliderFloat("Sharpness", &NISSettings.Sharpness, 0, 1, "%.2f", ImGuiSliderFlags_AlwaysClamp);

							ImGui::TreePop();
						}
					}

					ImGui::Checkbox("Chromatic Aberration", &postProcessingSetttings.IsChromaticAberrationEnabled);

					if (ImGui::TreeNodeEx("Bloom", ImGuiTreeNodeFlags_DefaultOpen)) {
						auto& bloomSettings = postProcessingSetttings.Bloom;

						{
							const ImGuiEx::ScopedID scopedID("Enable Bloom");

							ImGui::Checkbox("Enable", &bloomSettings.IsEnabled);
						}

						if (bloomSettings.IsEnabled) ImGui::SliderFloat("Strength", &bloomSettings.Strength, 0, 1, "%.2f", ImGuiSliderFlags_AlwaysClamp);

						ImGui::TreePop();
					}

					if (ImGui::TreeNodeEx("Tone Mapping", ImGuiTreeNodeFlags_DefaultOpen)) {
						auto& toneMappingSettings = postProcessingSetttings.ToneMapping;

						if (m_deviceResources->IsHDREnabled()) {
							auto& HDRSettings = toneMappingSettings.HDR;

							ImGui::SliderFloat("Paper White Nits", &HDRSettings.PaperWhiteNits, HDRSettings.MinPaperWhiteNits, HDRSettings.MaxPaperWhiteNits, "%.1f", ImGuiSliderFlags_AlwaysClamp);

							if (ImGui::BeginCombo("Color Primary Rotation", ToString(HDRSettings.ColorPrimaryRotation))) {
								for (const auto ColorPrimaryRotation : { ToneMapPostProcess::HDTV_to_UHDTV, ToneMapPostProcess::DCI_P3_D65_to_UHDTV, ToneMapPostProcess::HDTV_to_DCI_P3_D65 }) {
									const auto isSelected = HDRSettings.ColorPrimaryRotation == ColorPrimaryRotation;

									if (ImGui::Selectable(ToString(ColorPrimaryRotation), isSelected)) {
										HDRSettings.ColorPrimaryRotation = ColorPrimaryRotation;
									}

									if (isSelected) ImGui::SetItemDefaultFocus();
								}

								ImGui::EndCombo();
							}
						}
						else {
							auto& nonHDRSettings = toneMappingSettings.NonHDR;

							if (ImGui::BeginCombo("Operator", ToString(nonHDRSettings.Operator))) {
								for (const auto Operator : { ToneMapPostProcess::None, ToneMapPostProcess::Saturate, ToneMapPostProcess::Reinhard, ToneMapPostProcess::ACESFilmic }) {
									const auto isSelected = nonHDRSettings.Operator == Operator;

									if (ImGui::Selectable(ToString(Operator), isSelected)) nonHDRSettings.Operator = Operator;

									if (isSelected) ImGui::SetItemDefaultFocus();
								}

								ImGui::EndCombo();
							}

							if (nonHDRSettings.Operator != ToneMapPostProcess::None) {
								ImGui::SliderFloat("Exposure", &nonHDRSettings.Exposure, nonHDRSettings.MinExposure, nonHDRSettings.MaxExposure, "%.2f", ImGuiSliderFlags_AlwaysClamp);
							}
						}

						ImGui::TreePop();
					}

					ImGui::TreePop();
				}

				ImGui::TreePop();
			}

			if (ImGui::TreeNode("UI")) {
				ImGui::Checkbox("Show on Startup", &g_UISettings.ShowOnStartup);

				ImGui::SliderFloat("Window Opacity", &g_UISettings.WindowOpacity, 0, 1, "%.2f", ImGuiSliderFlags_AlwaysClamp);

				ImGui::TreePop();
			}

			if (ImGui::TreeNode("Controls")) {
				if (ImGui::TreeNodeEx("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
					auto& cameraSettings = g_controlsSettings.Camera;

					if (ImGui::TreeNodeEx("Speed", ImGuiTreeNodeFlags_DefaultOpen)) {
						auto& speedSettings = cameraSettings.Speed;

						ImGui::SliderFloat("Movement", &speedSettings.Movement, 0.0f, speedSettings.MaxMovement, "%.1f", ImGuiSliderFlags_AlwaysClamp);

						ImGui::SliderFloat("Rotation", &speedSettings.Rotation, 0.0f, speedSettings.MaxRotation, "%.2f", ImGuiSliderFlags_AlwaysClamp);

						ImGui::TreePop();
					}

					ImGui::TreePop();
				}

				ImGui::TreePop();
			}
		}

		ImGui::End();
	}

	void RenderPopupModalWindow(string_view popupModalName) {
		const auto PopupModal = [&](LPCSTR name, const auto& lambda) {
			if (name == popupModalName) ImGui::OpenPopup(name);

			ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetWorkCenter(), ImGuiCond_Always, { 0.5f, 0.5f });
			ImGui::SetNextWindowSize({});

			if (ImGui::BeginPopupModal(name, nullptr, ImGuiWindowFlags_HorizontalScrollbar)) {
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

		AddContents(
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

		AddContents(
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

		if (const auto label = "Loading Scene"; ImGui::Begin(label, nullptr, ImGuiWindowFlags_NoTitleBar)) {
			{
				const auto radius = static_cast<float>(GetOutputSize().cy) * 0.01f;
				ImGuiEx::Spinner(label, ImGui::GetColorU32(ImGuiCol_Button), radius, radius * 0.4f);
			}

			ImGui::SameLine();

			ImGui::Text(label);
		}

		ImGui::End();
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
