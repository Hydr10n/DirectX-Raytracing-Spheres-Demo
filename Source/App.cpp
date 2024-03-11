module;

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

#include "rtxdi/ReSTIRDI.h"

#include "sl_helpers.h"
#include "sl_dlss_g.h"

#include "NRD.h"

#include "ffx_fsr2.h"

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
import HaltonSamplePattern;
import LightPreparation;
import Material;
import Model;
import MyScene;
import PostProcessing.ChromaticAberration;
import PostProcessing.DenoisedComposition;
import Raytracing;
import RenderTexture;
import RTXDIResources;
import Scene;
import SharedData;
import StepTimer;
import StringConverters;
import Texture;
import ThreadHelpers;

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

			m_deviceResources->EnableVSync(g_graphicsSettings.IsVSyncEnabled);

			m_deviceResources->CreateDeviceResources();
			CreateDeviceDependentResources();

			m_deviceResources->CreateWindowSizeDependentResources();
			CreateWindowSizeDependentResources();
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
		if (const auto pFuture = m_futures.find(FutureNames::Scene); pFuture != cend(m_futures) && pFuture->second.wait_for(0s) == future_status::ready) m_futures.erase(pFuture);

		ignore = m_streamline->NewFrame();

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

	void OnWindowSizeChanged() { if (m_deviceResources->ResizeWindow(m_windowModeHelper.GetResolution())) CreateWindowSizeDependentResources(); }

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

		m_RTXDIResources = {};

		m_scene.reset();

		m_GPUBuffers = {};

		m_renderTextures = {};

		m_alphaBlending.reset();

		for (auto& toneMapping : m_toneMapping) toneMapping.reset();

		m_bloom = {};

		m_chromaticAberration.reset();

		m_FSR.reset();

		m_denoisedComposition.reset();
		m_NRD.reset();

		m_streamline.reset();

		m_lightPreparation.reset();

		m_raytracing.reset();

		m_renderDescriptorHeap.reset();
		m_resourceDescriptorHeap.reset();

		m_graphicsMemory.reset();
	}

	void OnDeviceRestored() override {
		CreateDeviceDependentResources();

		CreateWindowSizeDependentResources();

		LoadScene();
	}

private:
	WindowModeHelper& m_windowModeHelper;

	unique_ptr<DeviceResources> m_deviceResources = make_unique<DeviceResources>(DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_UNKNOWN, 2, D3D_FEATURE_LEVEL_12_1, D3D12_RAYTRACING_TIER_1_1, DeviceResources::c_AllowTearing | DeviceResources::c_DisableGpuTimeout);

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

	XMUINT2 m_renderSize{};

	HaltonSamplePattern m_haltonSamplePattern;

	struct FutureNames {
		MAKE_NAME(Scene);
	};
	map<string, future<void>, less<>> m_futures;

	exception_ptr m_exception;
	mutex m_exceptionMutex;

	struct ResourceDescriptorHeapIndex {
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
			InPreviousNormalRoughness, OutPreviousNormalRoughness,
			InNormalRoughness, OutNormalRoughness,
			InPreviousGeometricNormals, OutPreviousGeometricNormals,
			InGeometricNormals, OutGeometricNormals,
			OutNoisyDiffuse, OutNoisySpecular,
			InDenoisedDiffuse, OutDenoisedDiffuse,
			InDenoisedSpecular, OutDenoisedSpecular,
			InValidation, OutValidation,
			InBlur1, InBlur2,
			InNeighborOffsets,
			InFont,
			Reserve,
			Count = 1 << 16
		};
	};
	unique_ptr<DescriptorHeapEx> m_resourceDescriptorHeap;

	struct RenderDescriptorHeapIndex {
		enum {
			Color,
			FinalColor,
			Blur1, Blur2,
			Count
		};
	};
	unique_ptr<DescriptorHeap> m_renderDescriptorHeap;

	unique_ptr<Raytracing> m_raytracing;

	unique_ptr<LightPreparation> m_lightPreparation;

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

	DLSSGOptions m_DLSSGOptions;

	CommonSettings m_NRDCommonSettings{ .isBaseColorMetalnessAvailable = true };
	ReblurSettings m_NRDReblurSettings{ .hitDistanceReconstructionMode = HitDistanceReconstructionMode::AREA_3X3, .enableAntiFirefly = true };
	RelaxSettings m_NRDRelaxSettings{ .hitDistanceReconstructionMode = HitDistanceReconstructionMode::AREA_3X3, .enableAntiFirefly = true };
	unique_ptr<NRD> m_NRD;
	unique_ptr<DenoisedComposition> m_denoisedComposition;

	FSRSettings m_FSRSettings;
	unique_ptr<FSR> m_FSR;

	unique_ptr<ChromaticAberration> m_chromaticAberration;

	struct {
		unique_ptr<BasicPostProcess> Extraction, Blur;
		unique_ptr<DualPostProcess> Combination;
	} m_bloom;

	unique_ptr<ToneMapPostProcess> m_toneMapping[ToneMapPostProcess::Operator_Max];

	unique_ptr<SpriteBatch> m_alphaBlending;

	struct RenderTextureNames {
		MAKE_NAME(Color);
		MAKE_NAME(FinalColor);
		MAKE_NAME(PreviousLinearDepth);
		MAKE_NAME(LinearDepth);
		MAKE_NAME(NormalizedDepth);
		MAKE_NAME(MotionVectors);
		MAKE_NAME(PreviousBaseColorMetalness);
		MAKE_NAME(BaseColorMetalness);
		MAKE_NAME(EmissiveColor);
		MAKE_NAME(PreviousNormalRoughness);
		MAKE_NAME(NormalRoughness);
		MAKE_NAME(PreviousGeometricNormals);
		MAKE_NAME(GeometricNormals);
		MAKE_NAME(NoisyDiffuse);
		MAKE_NAME(NoisySpecular);
		MAKE_NAME(DenoisedDiffuse);
		MAKE_NAME(DenoisedSpecular);
		MAKE_NAME(Validation);
		MAKE_NAME(Blur1);
		MAKE_NAME(Blur2);
	};
	map<string, shared_ptr<RenderTexture>, less<>> m_renderTextures;

	struct {
		shared_ptr<ConstantBuffer<Camera>> Camera;
		shared_ptr<ConstantBuffer<SceneData>> SceneData;
		shared_ptr<UploadBuffer<InstanceData>> InstanceData;
		shared_ptr<UploadBuffer<ObjectData>> ObjectData;
	} m_GPUBuffers;

	CameraController m_cameraController;

	shared_ptr<Scene> m_scene;

	RTXDIResources m_RTXDIResources;
	atomic_bool m_RTXDIResourcesLock;

	struct { bool IsVisible, HasFocus = true, IsSettingsWindowOpen; } m_UIStates{};

	void CreateDeviceDependentResources() {
		const auto device = m_deviceResources->GetDevice();

		m_graphicsMemory = make_unique<GraphicsMemory>(device);

		CreateDescriptorHeaps();

		CreatePipelineStates();

		CreateConstantBuffers();
	}

	void CreateWindowSizeDependentResources() {
		const auto device = m_deviceResources->GetDevice();
		const auto commandList = m_deviceResources->GetCommandList();

		const auto outputSize = GetOutputSize();

		{
			const auto CreateTexture = [&](DXGI_FORMAT format, SIZE size, LPCSTR textureName, UINT srvDescriptorHeapIndex = ~0u, UINT uavDescriptorHeapIndex = ~0u, UINT rtvDescriptorHeapIndex = ~0u) {
				auto& texture = m_renderTextures[textureName];
				texture = make_shared<RenderTexture>(format);
				texture->SetDevice(device, m_resourceDescriptorHeap.get(), srvDescriptorHeapIndex, uavDescriptorHeapIndex, m_renderDescriptorHeap.get(), rtvDescriptorHeapIndex);
				texture->CreateResource(size.cx, size.cy);
			};
			const auto CreateTexture1 = [&](DXGI_FORMAT format, LPCSTR textureName, UINT srvDescriptorHeapIndex = ~0u, UINT uavDescriptorHeapIndex = ~0u, UINT rtvDescriptorHeapIndex = ~0u) {
				CreateTexture(format, outputSize, textureName, srvDescriptorHeapIndex, uavDescriptorHeapIndex, rtvDescriptorHeapIndex);
			};

			CreateTexture1(DXGI_FORMAT_R16G16B16A16_FLOAT, RenderTextureNames::Color, ResourceDescriptorHeapIndex::InColor, ResourceDescriptorHeapIndex::OutColor, RenderDescriptorHeapIndex::Color);
			CreateTexture1(DXGI_FORMAT_R16G16B16A16_FLOAT, RenderTextureNames::FinalColor, ResourceDescriptorHeapIndex::InFinalColor, ResourceDescriptorHeapIndex::OutFinalColor, RenderDescriptorHeapIndex::FinalColor);
			CreateTexture1(DXGI_FORMAT_R32_FLOAT, RenderTextureNames::PreviousLinearDepth, ResourceDescriptorHeapIndex::InPreviousLinearDepth, ResourceDescriptorHeapIndex::OutPreviousLinearDepth);
			CreateTexture1(DXGI_FORMAT_R32_FLOAT, RenderTextureNames::LinearDepth, ResourceDescriptorHeapIndex::InLinearDepth, ResourceDescriptorHeapIndex::OutLinearDepth);
			CreateTexture1(DXGI_FORMAT_R32_FLOAT, RenderTextureNames::NormalizedDepth, ResourceDescriptorHeapIndex::InNormalizedDepth, ResourceDescriptorHeapIndex::OutNormalizedDepth);
			CreateTexture1(DXGI_FORMAT_R16G16B16A16_FLOAT, RenderTextureNames::MotionVectors, ResourceDescriptorHeapIndex::InMotionVectors, ResourceDescriptorHeapIndex::OutMotionVectors);
			CreateTexture1(DXGI_FORMAT_R16G16B16A16_FLOAT, RenderTextureNames::PreviousBaseColorMetalness, ResourceDescriptorHeapIndex::InPreviousBaseColorMetalness, ResourceDescriptorHeapIndex::OutPreviousBaseColorMetalness);
			CreateTexture1(DXGI_FORMAT_R8G8B8A8_UNORM, RenderTextureNames::BaseColorMetalness, ResourceDescriptorHeapIndex::InBaseColorMetalness, ResourceDescriptorHeapIndex::OutBaseColorMetalness);
			CreateTexture1(DXGI_FORMAT_R11G11B10_FLOAT, RenderTextureNames::EmissiveColor, ResourceDescriptorHeapIndex::InEmissiveColor, ResourceDescriptorHeapIndex::OutEmissiveColor);

			{
				const auto normalFormat = NRD::ToDXGIFormat(GetLibraryDesc().normalEncoding);
				CreateTexture1(normalFormat, RenderTextureNames::PreviousNormalRoughness, ResourceDescriptorHeapIndex::InPreviousNormalRoughness, ResourceDescriptorHeapIndex::OutPreviousNormalRoughness);
				CreateTexture1(normalFormat, RenderTextureNames::NormalRoughness, ResourceDescriptorHeapIndex::InNormalRoughness, ResourceDescriptorHeapIndex::OutNormalRoughness);
			}

			CreateTexture1(DXGI_FORMAT_R16G16_SNORM, RenderTextureNames::PreviousGeometricNormals, ResourceDescriptorHeapIndex::InPreviousGeometricNormals, ResourceDescriptorHeapIndex::OutPreviousGeometricNormals);
			CreateTexture1(DXGI_FORMAT_R16G16_SNORM, RenderTextureNames::GeometricNormals, ResourceDescriptorHeapIndex::InGeometricNormals, ResourceDescriptorHeapIndex::OutGeometricNormals);

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
				CreateTexture1(DXGI_FORMAT_R16G16B16A16_FLOAT, RenderTextureNames::NoisyDiffuse, ~0u, ResourceDescriptorHeapIndex::OutNoisyDiffuse);
				CreateTexture1(DXGI_FORMAT_R16G16B16A16_FLOAT, RenderTextureNames::NoisySpecular, ~0u, ResourceDescriptorHeapIndex::OutNoisySpecular);
				CreateTexture1(DXGI_FORMAT_R16G16B16A16_FLOAT, RenderTextureNames::DenoisedDiffuse, ResourceDescriptorHeapIndex::InDenoisedDiffuse, ResourceDescriptorHeapIndex::OutDenoisedDiffuse);
				CreateTexture1(DXGI_FORMAT_R16G16B16A16_FLOAT, RenderTextureNames::DenoisedSpecular, ResourceDescriptorHeapIndex::InDenoisedSpecular, ResourceDescriptorHeapIndex::OutDenoisedSpecular);
				CreateTexture1(DXGI_FORMAT_R8G8B8A8_UNORM, RenderTextureNames::Validation, ResourceDescriptorHeapIndex::InValidation, ResourceDescriptorHeapIndex::OutValidation);
			}

			m_FSR = make_unique<FSR>(device, commandList, FfxDimensions2D{ static_cast<uint32_t>(outputSize.cx), static_cast<uint32_t>(outputSize.cy) }, FFX_FSR2_ENABLE_HIGH_DYNAMIC_RANGE | FFX_FSR2_ENABLE_DEPTH_INVERTED | FFX_FSR2_ENABLE_DEPTH_INFINITE | FFX_FSR2_ENABLE_AUTO_EXPOSURE);

			{
				const SIZE size{ outputSize.cx / 2, outputSize.cy / 2 };
				CreateTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, size, RenderTextureNames::Blur1, ResourceDescriptorHeapIndex::InBlur1, ~0u, RenderDescriptorHeapIndex::Blur1);
				CreateTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, size, RenderTextureNames::Blur2, ResourceDescriptorHeapIndex::InBlur2, ~0u, RenderDescriptorHeapIndex::Blur2);
			}

			SelectSuperResolutionUpscaler();
			SetSuperResolutionOptions();
		}

		m_cameraController.SetLens(XMConvertToRadians(g_graphicsSettings.Camera.HorizontalFieldOfView), static_cast<float>(outputSize.cx) / static_cast<float>(outputSize.cy));

		{
			const auto& IO = ImGui::GetIO();

			if (IO.BackendRendererUserData != nullptr) ImGui_ImplDX12_Shutdown();
			ImGui_ImplDX12_Init(device, m_deviceResources->GetBackBufferCount(), m_deviceResources->GetBackBufferFormat(), m_resourceDescriptorHeap->Heap(), m_resourceDescriptorHeap->GetCpuHandle(ResourceDescriptorHeapIndex::InFont), m_resourceDescriptorHeap->GetGpuHandle(ResourceDescriptorHeapIndex::InFont));

			IO.Fonts->Clear();
			IO.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/segoeui.ttf", static_cast<float>(outputSize.cy) * 0.022f);
		}
	}

	void Update() {
		const auto isReflexAvailable = m_streamline->IsFeatureAvailable(kFeatureReflex);

		if (isReflexAvailable) ignore = m_streamline->SetReflexMarker(ReflexMarker::eSimulationStart);

		{
			auto& camera = m_GPUBuffers.Camera->At(0);

			camera.PreviousWorldToView = m_cameraController.GetWorldToView();
			camera.PreviousViewToProjection = m_cameraController.GetViewToProjection();
			camera.PreviousWorldToProjection = m_cameraController.GetWorldToProjection();
			camera.PreviousProjectionToView = m_cameraController.GetProjectionToView();
			camera.PreviousViewToWorld = m_cameraController.GetViewToWorld();

			ProcessInput();

			camera.Position = m_cameraController.GetPosition();
			camera.RightDirection = m_cameraController.GetRightDirection();
			camera.UpDirection = m_cameraController.GetUpDirection();
			camera.ForwardDirection = m_cameraController.GetForwardDirection();
			camera.NearDepth = m_cameraController.GetNearDepth();
			camera.FarDepth = m_cameraController.GetFarDepth();
			camera.Jitter = g_graphicsSettings.Camera.IsJitterEnabled ? m_haltonSamplePattern.GetNext() : XMFLOAT2();
			camera.WorldToProjection = m_cameraController.GetWorldToProjection();
		}

		if (IsSceneReady()) UpdateScene();

		if (isReflexAvailable) ignore = m_streamline->SetReflexMarker(ReflexMarker::eSimulationEnd);
	}

	void Render() {
		if (!m_stepTimer.GetFrameCount()) return;

		m_deviceResources->Prepare();

		const auto commandList = m_deviceResources->GetCommandList();

		const auto isReflexAvailable = m_streamline->IsFeatureAvailable(kFeatureReflex);

		if (isReflexAvailable) ignore = m_streamline->SetReflexMarker(ReflexMarker::eRenderSubmitStart);

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
				if (m_DLSSGOptions.mode == DLSSGMode::eOff && IsDLSSFrameGenerationEnabled()) SetFrameGenerationOptions();

				if (!m_scene->IsStatic()) m_scene->CreateAccelerationStructures(true);

				if (g_graphicsSettings.Raytracing.RTXDI.IsEnabled && m_lightPreparation->GetEmissiveTriangleCount()) {
					m_RTXDIResources.ReSTIRDIContext->setFrameIndex(m_stepTimer.GetFrameCount() - 1);
					PrepareLights(commandList);
				}

				RenderScene();

				PostProcessGraphics();
			}
			else if (m_DLSSGOptions.mode != DLSSGMode::eOff) SetFrameGenerationOptions(false);

			if (m_UIStates.IsVisible) RenderUI();
		}

		if (isReflexAvailable) ignore = m_streamline->SetReflexMarker(ReflexMarker::eRenderSubmitEnd);

		if (isReflexAvailable) ignore = m_streamline->SetReflexMarker(ReflexMarker::ePresentStart);

		m_deviceResources->Present();

		if (isReflexAvailable) ignore = m_streamline->SetReflexMarker(ReflexMarker::ePresentEnd);

		m_deviceResources->WaitForGpu();

		m_graphicsMemory->Commit(m_deviceResources->GetCommandQueue());

		{
			swap(m_renderTextures.at(RenderTextureNames::PreviousLinearDepth), m_renderTextures.at(RenderTextureNames::LinearDepth));
			swap(m_renderTextures.at(RenderTextureNames::PreviousBaseColorMetalness), m_renderTextures.at(RenderTextureNames::BaseColorMetalness));
			swap(m_renderTextures.at(RenderTextureNames::PreviousNormalRoughness), m_renderTextures.at(RenderTextureNames::NormalRoughness));
			swap(m_renderTextures.at(RenderTextureNames::PreviousGeometricNormals), m_renderTextures.at(RenderTextureNames::GeometricNormals));
		}
	}

	bool IsSceneLoading() const { return m_futures.contains(FutureNames::Scene); }
	bool IsSceneReady() const { return !IsSceneLoading() && m_scene; }

	void LoadScene() {
		m_futures[FutureNames::Scene] = StartDetachedFuture([&] {
			try {
			UINT descriptorHeapIndex = ResourceDescriptorHeapIndex::Reserve;
			m_scene = make_shared<MyScene>(m_deviceResources->GetDevice(), m_deviceResources->GetCommandQueue());
			m_scene->Load(MySceneDesc(), *m_resourceDescriptorHeap, descriptorHeapIndex);

			CreateStructuredBuffers();

			ResetCamera();

			m_raytracing->SetScene(m_scene.get());

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

		m_resourceDescriptorHeap = make_unique<DescriptorHeapEx>(device, ResourceDescriptorHeapIndex::Count, ResourceDescriptorHeapIndex::Reserve);

		m_renderDescriptorHeap = make_unique<DescriptorHeap>(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, RenderDescriptorHeapIndex::Count);
	}

	void CreatePipelineStates() {
		const auto device = m_deviceResources->GetDevice();

		m_raytracing = make_unique<Raytracing>(device);

		CreatePostProcessing();
	}

	void CreatePostProcessing() {
		const auto device = m_deviceResources->GetDevice();

		m_lightPreparation = make_unique<LightPreparation>(device);

		{
			ignore = slSetD3DDevice(device);

			DXGI_ADAPTER_DESC adapterDesc;
			ThrowIfFailed(m_deviceResources->GetAdapter()->GetDesc(&adapterDesc));
			m_streamline = make_unique<Streamline>(m_deviceResources->GetCommandList(), &adapterDesc.AdapterLuid, sizeof(adapterDesc.AdapterLuid), 0);

			if (m_streamline->IsFeatureAvailable(kFeatureReflex)) SetReflexOptions();
		}

		m_denoisedComposition = make_unique<DenoisedComposition>(device);

		m_chromaticAberration = make_unique<ChromaticAberration>(device);

		{
			const RenderTargetState renderTargetState(DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_UNKNOWN);

			m_bloom = {
				.Extraction = make_unique<BasicPostProcess>(device, renderTargetState, BasicPostProcess::BloomExtract),
				.Blur = make_unique<BasicPostProcess>(device, renderTargetState, BasicPostProcess::BloomBlur),
				.Combination = make_unique<DualPostProcess>(device, renderTargetState, DualPostProcess::BloomCombine),
			};
		}

		for (const auto toneMappingOperator : { ToneMapPostProcess::None, ToneMapPostProcess::Saturate, ToneMapPostProcess::Reinhard, ToneMapPostProcess::ACESFilmic }) {
			m_toneMapping[toneMappingOperator] = make_unique<ToneMapPostProcess>(device, RenderTargetState(m_deviceResources->GetBackBufferFormat(), DXGI_FORMAT_UNKNOWN), toneMappingOperator, toneMappingOperator == ToneMapPostProcess::None ? ToneMapPostProcess::Linear : ToneMapPostProcess::SRGB);
		}

		{
			ResourceUploadBatch resourceUploadBatch(device);
			resourceUploadBatch.Begin();

			m_alphaBlending = make_unique<SpriteBatch>(device, resourceUploadBatch, SpriteBatchPipelineStateDescription(RenderTargetState(m_deviceResources->GetBackBufferFormat(), DXGI_FORMAT_UNKNOWN), &CommonStates::NonPremultiplied));

			resourceUploadBatch.End(m_deviceResources->GetCommandQueue()).get();
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
		m_lightPreparation->SetScene(m_scene.get());
		if (const auto emissiveTriangleCount = m_lightPreparation->GetEmissiveTriangleCount()) {
			const auto device = m_deviceResources->GetDevice();

			m_RTXDIResources.CreateLightBuffers(device, emissiveTriangleCount, m_scene->GetObjectCount());
			if (!m_RTXDIResourcesLock) m_RTXDIResources.CreateDIReservoir(device);

			{
				ResourceUploadBatch resourceUploadBatch(device);
				resourceUploadBatch.Begin();

				m_lightPreparation->PrepareResources(resourceUploadBatch, *m_RTXDIResources.LightIndices);

				resourceUploadBatch.End(m_deviceResources->GetCommandQueue()).get();
			}
		}
	}

	void PrepareLights(ID3D12GraphicsCommandList* pCommandList) {
		m_lightPreparation->GPUBuffers = {
			.InInstanceData = m_GPUBuffers.InstanceData.get(),
			.InObjectData = m_GPUBuffers.ObjectData.get(),
			.OutLightInfo = m_RTXDIResources.LightInfo.get()
		};

		m_lightPreparation->Process(pCommandList);
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

			const auto IsCubeMap = [](ID3D12Resource* pResource) {
				if (pResource == nullptr) return false;
				const auto desc = pResource->GetDesc();
				return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.DepthOrArraySize == 6;
			};
			if (m_scene->EnvironmentLightTexture.DescriptorHeapIndices.SRV != ~0u) {
				sceneData.IsEnvironmentLightTextureCubeMap = IsCubeMap(m_scene->EnvironmentLightTexture.Resource.Get());
				XMStoreFloat3x4(&sceneData.EnvironmentLightTextureTransform, m_scene->EnvironmentLightTexture.Transform());
			}
			if (m_scene->EnvironmentTexture.DescriptorHeapIndices.SRV) {
				sceneData.IsEnvironmentTextureCubeMap = IsCubeMap(m_scene->EnvironmentTexture.Resource.Get());
				XMStoreFloat3x4(&sceneData.EnvironmentTextureTransform, m_scene->EnvironmentTexture.Transform());
			}
			sceneData.ResourceDescriptorHeapIndices = {
				.InEnvironmentLightTexture = m_scene->EnvironmentLightTexture.DescriptorHeapIndices.SRV,
				.InEnvironmentTexture = m_scene->EnvironmentTexture.DescriptorHeapIndices.SRV
			};

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

			auto& resourceDescriptorHeapIndices = objectData.ResourceDescriptorHeapIndices;
			resourceDescriptorHeapIndices = {
				.Mesh{
					.Vertices = mesh->DescriptorHeapIndices.Vertices,
					.Indices = mesh->DescriptorHeapIndices.Indices
				}
			};

			for (const auto& [TextureType, Texture] : renderObject.Textures) {
				const auto index = Texture.DescriptorHeapIndices.SRV;
				switch (auto& indices = resourceDescriptorHeapIndices.Textures; TextureType) {
					case TextureType::BaseColorMap: indices.BaseColorMap = index; break;
					case TextureType::EmissiveColorMap: indices.EmissiveColorMap = index; break;
					case TextureType::MetallicMap: indices.MetallicMap = index; break;
					case TextureType::RoughnessMap: indices.RoughnessMap = index; break;
					case TextureType::AmbientOcclusionMap: indices.AmbientOcclusionMap = index; break;
					case TextureType::TransmissionMap: indices.TransmissionMap = index; break;
					case TextureType::OpacityMap: indices.OpacityMap = index; break;
					case TextureType::NormalMap: indices.NormalMap = index; break;
					default: Throw<out_of_range>("Unsupported texture type");
				}
			}
		}
	}

	void RenderScene() {
		const auto commandList = m_deviceResources->GetCommandList();

		const auto& raytracingSettings = g_graphicsSettings.Raytracing;
		const auto& RTXDISettings = raytracingSettings.RTXDI;
		const auto& reSTIRDIContext = *m_RTXDIResources.ReSTIRDIContext;
		m_raytracing->SetConstants({
			.RenderSize = m_renderSize,
			.FrameIndex = m_stepTimer.GetFrameCount() - 1,
			.MaxNumberOfBounces = raytracingSettings.MaxNumberOfBounces,
			.SamplesPerPixel = raytracingSettings.SamplesPerPixel,
			.IsRussianRouletteEnabled = raytracingSettings.IsRussianRouletteEnabled,
			.RTXDI{
				.IsEnabled = RTXDISettings.IsEnabled && m_lightPreparation->GetEmissiveTriangleCount(),
				.LocalLightSamples = RTXDISettings.LocalLightSamples,
				.BRDFSamples = RTXDISettings.BRDFSamples,
				.SpatioTemporalSamples = RTXDISettings.SpatioTemporalSamples,
				.InputBufferIndex = !(reSTIRDIContext.getFrameIndex() & 1),
				.OutputBufferIndex = reSTIRDIContext.getFrameIndex() & 1,
				.UniformRandomNumber = reSTIRDIContext.getTemporalResamplingParameters().uniformRandomNumber,
				.LightBufferParameters = m_lightPreparation->GetLightBufferParameters(),
				.RuntimeParameters{
					.neighborOffsetMask = reSTIRDIContext.getStaticParameters().NeighborOffsetCount - 1
				},
				.ReservoirBufferParameters = reSTIRDIContext.getReservoirBufferParameters()
			},
			.NRD{
				.Denoiser = m_NRD->IsAvailable() ? g_graphicsSettings.PostProcessing.NRD.Denoiser : NRDDenoiser::None,
				.HitDistanceParameters = reinterpret_cast<const XMFLOAT4&>(m_NRDReblurSettings.hitDistanceParameters)
			}
			});

		m_raytracing->GPUBuffers = {
			.InSceneData = m_GPUBuffers.SceneData.get(),
			.InCamera = m_GPUBuffers.Camera.get(),
			.InInstanceData = m_GPUBuffers.InstanceData.get(),
			.InObjectData = m_GPUBuffers.ObjectData.get(),
			.InLightInfo = m_RTXDIResources.LightInfo.get(),
			.InLightIndices = m_RTXDIResources.LightIndices.get(),
			.OutDIReservoir = m_RTXDIResources.DIReservoir.get()
		};

		const auto
			& previousLinearDepth = *m_renderTextures.at(RenderTextureNames::PreviousLinearDepth),
			& previousBaseColorMetalness = *m_renderTextures.at(RenderTextureNames::PreviousBaseColorMetalness),
			& previousNormalRoughness = *m_renderTextures.at(RenderTextureNames::PreviousNormalRoughness),
			& previousGeometricNormals = *m_renderTextures.at(RenderTextureNames::PreviousGeometricNormals);

		const ScopedBarrier scopedBarrier(
			commandList,
			{
				CD3DX12_RESOURCE_BARRIER::Transition(previousLinearDepth.GetResource(), previousLinearDepth.GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(previousBaseColorMetalness.GetResource(), previousBaseColorMetalness.GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(previousNormalRoughness.GetResource(), previousNormalRoughness.GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(previousGeometricNormals.GetResource(), previousGeometricNormals.GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE)
			}
		);

		const auto GetSRV = [&](const RenderTexture& texture) { return m_resourceDescriptorHeap->GetGpuHandle(texture.GetSrvDescriptorHeapIndex()); };
		const auto GetUAV = [&](LPCSTR name) { return m_resourceDescriptorHeap->GetGpuHandle(m_renderTextures.at(name)->GetUavDescriptorHeapIndex()); };
		m_raytracing->GPUDescriptors = {
			.InPreviousLinearDepth = GetSRV(previousLinearDepth),
			.InPreviousBaseColorMetalness = GetSRV(previousBaseColorMetalness),
			.InPreviousNormalRoughness = GetSRV(previousNormalRoughness),
			.InPreviousGeometricNormals = GetSRV(previousGeometricNormals),
			.OutColor = GetUAV(RenderTextureNames::Color),
			.OutLinearDepth = GetUAV(RenderTextureNames::LinearDepth),
			.OutNormalizedDepth = GetUAV(RenderTextureNames::NormalizedDepth),
			.OutMotionVectors = GetUAV(RenderTextureNames::MotionVectors),
			.OutBaseColorMetalness = GetUAV(RenderTextureNames::BaseColorMetalness),
			.OutEmissiveColor = GetUAV(RenderTextureNames::EmissiveColor),
			.OutNormalRoughness = GetUAV(RenderTextureNames::NormalRoughness),
			.OutGeometricNormals = GetUAV(RenderTextureNames::GeometricNormals),
			.OutNoisyDiffuse = GetUAV(RenderTextureNames::NoisyDiffuse),
			.OutNoisySpecular = GetUAV(RenderTextureNames::NoisySpecular),
			.InNeighborOffsets = m_resourceDescriptorHeap->GetGpuHandle(ResourceDescriptorHeapIndex::InNeighborOffsets)
		};

		m_raytracing->Render(commandList);
	}

	bool IsDLSSSuperResolutionEnabled() const { return g_graphicsSettings.PostProcessing.SuperResolution.Upscaler == Upscaler::DLSS && m_streamline->IsFeatureAvailable(kFeatureDLSS); }
	bool IsDLSSFrameGenerationEnabled() const { return g_graphicsSettings.PostProcessing.IsDLSSFrameGenerationEnabled && m_streamline->IsFeatureAvailable(kFeatureDLSS_G) && IsReflexEnabled(); }
	bool IsFSRSuperResolutionEnabled() const { return g_graphicsSettings.PostProcessing.SuperResolution.Upscaler == Upscaler::FSR && m_FSR->IsAvailable(); }
	bool IsNISEnabled() const { return g_graphicsSettings.PostProcessing.NIS.IsEnabled && m_streamline->IsFeatureAvailable(kFeatureNIS); }
	bool IsNRDEnabled() const { return g_graphicsSettings.PostProcessing.NRD.Denoiser != NRDDenoiser::None && m_NRD->IsAvailable(); }
	bool IsReflexEnabled() const { return g_graphicsSettings.ReflexMode != ReflexMode::eOff && m_streamline->IsFeatureAvailable(kFeatureReflex); }

	static void SetReflexOptions() {
		ReflexOptions options;
		options.mode = g_graphicsSettings.ReflexMode;
		ignore = slReflexSetOptions(options);
	}

	auto CreateResourceTagInfo(BufferType type, const RenderTexture& texture, bool isRenderSize = true, ResourceLifecycle lifecycle = ResourceLifecycle::eValidUntilPresent) const {
		ResourceTagInfo resourceTagInfo{
			.Type = type,
			.Resource = Resource(sl::ResourceType::eTex2d, texture.GetResource(), texture.GetState()),
			.Lifecycle = lifecycle
		};
		if (isRenderSize) resourceTagInfo.Extent = { .width = m_renderSize.x, .height = m_renderSize.y };
		return resourceTagInfo;
	}

	void SelectSuperResolutionUpscaler() {
		if (auto& upscaler = g_graphicsSettings.PostProcessing.SuperResolution.Upscaler;
			upscaler == Upscaler::DLSS) {
			if (!m_streamline->IsFeatureAvailable(kFeatureDLSS)) {
				if (m_FSR->IsAvailable()) upscaler = Upscaler::FSR;
				else upscaler = Upscaler::None;
			}
		}
		else if (upscaler == Upscaler::FSR) {
			if (!m_FSR->IsAvailable()) {
				if (m_streamline->IsFeatureAvailable(kFeatureDLSS)) upscaler = Upscaler::DLSS;
				else upscaler = Upscaler::None;
			}
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
				slDLSSGetOptimalSettings(options, optimalSettings);
				ignore = m_streamline->SetConstants(options);
				m_renderSize = XMUINT2(optimalSettings.optimalRenderWidth, optimalSettings.optimalRenderHeight);
			}
			break;

			case Upscaler::FSR:
			{
				auto isNative = false;
				FfxFsr2QualityMode mode;
				switch (SelectSuperResolutionMode()) {
					case SuperResolutionMode::Native: isNative = true; break;
					case SuperResolutionMode::Quality: mode = FFX_FSR2_QUALITY_MODE_QUALITY; break;
					case SuperResolutionMode::Balanced: mode = FFX_FSR2_QUALITY_MODE_BALANCED; break;
					case SuperResolutionMode::Performance: mode = FFX_FSR2_QUALITY_MODE_PERFORMANCE; break;
					case SuperResolutionMode::UltraPerformance: mode = FFX_FSR2_QUALITY_MODE_ULTRA_PERFORMANCE; break;
					default: throw;
				}
				if (isNative) m_renderSize = XMUINT2(outputSize.cx, outputSize.cy);
				else {
					ignore = ffxFsr2GetRenderResolutionFromQualityMode(&m_FSRSettings.RenderSize.width, &m_FSRSettings.RenderSize.height, outputSize.cx, outputSize.cy, mode);
					m_renderSize = XMUINT2(m_FSRSettings.RenderSize.width, m_FSRSettings.RenderSize.height);
				}
			}
			break;
		}

		OnRenderSizeChanged();
	}

	void SetFrameGenerationOptions(bool enable = g_graphicsSettings.PostProcessing.IsDLSSFrameGenerationEnabled && g_graphicsSettings.ReflexMode != ReflexMode::eOff) {
		m_DLSSGOptions.mode = enable ? DLSSGMode::eAuto : DLSSGMode::eOff;
		ignore = m_streamline->SetConstants(m_DLSSGOptions);
	}

	void ResetTemporalAccumulation() {
		m_slConstants.reset = Boolean::eTrue;

		m_NRDCommonSettings.accumulationMode = AccumulationMode::CLEAR_AND_RESTART;

		m_FSRSettings.Reset = true;
	}

	void OnRenderSizeChanged() {
		const auto outputSize = GetOutputSize();
		m_haltonSamplePattern = HaltonSamplePattern(static_cast<UINT>(8 * (static_cast<float>(outputSize.cx) / static_cast<float>(m_renderSize.x)) * (static_cast<float>(outputSize.cy) / static_cast<float>(m_renderSize.y))));

		ResetTemporalAccumulation();

		{
			const struct ScopedAtomic {
				atomic_bool& Value;
				ScopedAtomic(atomic_bool& value) : Value(value) { Value = true; }
				~ScopedAtomic() { Value = false; }
			} lock = m_RTXDIResourcesLock;

			const auto device = m_deviceResources->GetDevice();

			ResourceUploadBatch resourceUploadBatch(device);
			resourceUploadBatch.Begin();

			m_RTXDIResources.ReSTIRDIContext = make_unique<ReSTIRDIContext>(ReSTIRDIStaticParameters{ .RenderWidth = m_renderSize.x, .RenderHeight = m_renderSize.y });
			m_RTXDIResources.CreateNeighborOffsets(device, resourceUploadBatch, m_resourceDescriptorHeap->GetCpuHandle(ResourceDescriptorHeapIndex::InNeighborOffsets));
			m_RTXDIResources.CreateDIReservoir(device);

			resourceUploadBatch.End(m_deviceResources->GetCommandQueue()).get();
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
		else if (IsFSRSuperResolutionEnabled()) {
			ProcessFSRSuperResolution();

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
		const auto& camera = m_GPUBuffers.Camera->At(0);
		m_slConstants.jitterOffset = { -camera.Jitter.x, -camera.Jitter.y };
		reinterpret_cast<XMFLOAT3&>(m_slConstants.cameraPos) = m_cameraController.GetPosition();
		reinterpret_cast<XMFLOAT3&>(m_slConstants.cameraUp) = m_cameraController.GetUpDirection();
		reinterpret_cast<XMFLOAT3&>(m_slConstants.cameraRight) = m_cameraController.GetRightDirection();
		reinterpret_cast<XMFLOAT3&>(m_slConstants.cameraFwd) = m_cameraController.GetForwardDirection();
		m_slConstants.cameraNear = m_cameraController.GetNearDepth();
		m_slConstants.cameraFar = m_cameraController.GetFarDepth();
		m_slConstants.cameraFOV = m_cameraController.GetVerticalFieldOfView();
		m_slConstants.cameraAspectRatio = m_cameraController.GetAspectRatio();
		reinterpret_cast<XMFLOAT4X4&>(m_slConstants.cameraViewToClip) = m_cameraController.GetViewToProjection();
		recalculateCameraMatrices(m_slConstants, reinterpret_cast<const float4x4&>(camera.PreviousViewToWorld), reinterpret_cast<const float4x4&>(camera.PreviousViewToProjection));

		m_slConstants.mvecScale = { 1 / static_cast<float>(m_renderSize.x), 1 / static_cast<float>(m_renderSize.y) };

		ignore = m_streamline->SetConstants(m_slConstants);

		m_slConstants.reset = Boolean::eFalse;
	}

	void ProcessNRD() {
		const auto commandList = m_deviceResources->GetCommandList();

		const auto& NRDSettings = g_graphicsSettings.PostProcessing.NRD;

		const auto
			& linearDepth = *m_renderTextures.at(RenderTextureNames::LinearDepth),
			& baseColorMetalness = *m_renderTextures.at(RenderTextureNames::BaseColorMetalness),
			& normalRoughness = *m_renderTextures.at(RenderTextureNames::NormalRoughness),
			& denoisedDiffuse = *m_renderTextures.at(RenderTextureNames::DenoisedDiffuse),
			& denoisedSpecular = *m_renderTextures.at(RenderTextureNames::DenoisedSpecular);

		{
			m_NRD->NewFrame();

			const auto Tag = [&](nrd::ResourceType resourceType, const RenderTexture& texture) { m_NRD->Tag(resourceType, texture.GetResource(), texture.GetState()); };
			Tag(nrd::ResourceType::IN_VIEWZ, linearDepth);
			Tag(nrd::ResourceType::IN_MV, *m_renderTextures.at(RenderTextureNames::MotionVectors));
			Tag(nrd::ResourceType::IN_BASECOLOR_METALNESS, baseColorMetalness);
			Tag(nrd::ResourceType::IN_NORMAL_ROUGHNESS, normalRoughness);
			Tag(nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST, *m_renderTextures.at(RenderTextureNames::NoisyDiffuse));
			Tag(nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST, *m_renderTextures.at(RenderTextureNames::NoisySpecular));
			Tag(nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST, denoisedDiffuse);
			Tag(nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST, denoisedSpecular);
			Tag(nrd::ResourceType::OUT_VALIDATION, *m_renderTextures.at(RenderTextureNames::Validation));

			const auto& camera = m_GPUBuffers.Camera->At(0);
			reinterpret_cast<XMFLOAT4X4&>(m_NRDCommonSettings.worldToViewMatrixPrev) = camera.PreviousWorldToView;
			reinterpret_cast<XMFLOAT4X4&>(m_NRDCommonSettings.viewToClipMatrixPrev) = camera.PreviousViewToProjection;
			reinterpret_cast<XMFLOAT4X4&>(m_NRDCommonSettings.worldToViewMatrix) = m_cameraController.GetWorldToView();
			reinterpret_cast<XMFLOAT4X4&>(m_NRDCommonSettings.viewToClipMatrix) = m_cameraController.GetViewToProjection();
			ranges::copy(m_NRDCommonSettings.cameraJitter, m_NRDCommonSettings.cameraJitterPrev);
			reinterpret_cast<XMFLOAT2&>(m_NRDCommonSettings.cameraJitter) = camera.Jitter;

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

			const auto& emissiveColor = *m_renderTextures.at(RenderTextureNames::EmissiveColor), & color = *m_renderTextures.at(RenderTextureNames::Color);

			const ScopedBarrier scopedBarrier(
				commandList,
				{
					CD3DX12_RESOURCE_BARRIER::Transition(linearDepth.GetResource(), linearDepth.GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(baseColorMetalness.GetResource(), baseColorMetalness.GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(emissiveColor.GetResource(), emissiveColor.GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(normalRoughness.GetResource(), normalRoughness.GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(denoisedDiffuse.GetResource(), denoisedDiffuse.GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(denoisedSpecular.GetResource(), denoisedSpecular.GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE)
				}
			);

			const auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(color.GetResource());
			commandList->ResourceBarrier(1, &barrier);

			m_denoisedComposition->GPUDescriptors = {
				.InLinearDepth = m_resourceDescriptorHeap->GetGpuHandle(linearDepth.GetSrvDescriptorHeapIndex()),
				.InBaseColorMetalness = m_resourceDescriptorHeap->GetGpuHandle(baseColorMetalness.GetSrvDescriptorHeapIndex()),
				.InEmissiveColor = m_resourceDescriptorHeap->GetGpuHandle(emissiveColor.GetSrvDescriptorHeapIndex()),
				.InNormalRoughness = m_resourceDescriptorHeap->GetGpuHandle(normalRoughness.GetSrvDescriptorHeapIndex()),
				.InDenoisedDiffuse = m_resourceDescriptorHeap->GetGpuHandle(denoisedDiffuse.GetSrvDescriptorHeapIndex()),
				.InDenoisedSpecular = m_resourceDescriptorHeap->GetGpuHandle(denoisedSpecular.GetSrvDescriptorHeapIndex()),
				.OutColor = m_resourceDescriptorHeap->GetGpuHandle(color.GetUavDescriptorHeapIndex())
			};

			m_denoisedComposition->Process(commandList);
		}
	}

	void ProcessDLSSSuperResolution() {
		ResourceTagInfo resourceTagInfos[]{
			CreateResourceTagInfo(kBufferTypeDepth, *m_renderTextures.at(RenderTextureNames::NormalizedDepth)),
			CreateResourceTagInfo(kBufferTypeMotionVectors, *m_renderTextures.at(RenderTextureNames::MotionVectors)),
			CreateResourceTagInfo(kBufferTypeScalingInputColor, *m_renderTextures.at(RenderTextureNames::Color)),
			CreateResourceTagInfo(kBufferTypeScalingOutputColor, *m_renderTextures.at(RenderTextureNames::FinalColor), false)
		};
		ignore = m_streamline->EvaluateFeature(kFeatureDLSS, CreateResourceTags(resourceTagInfos));

		const auto descriptorHeap = m_resourceDescriptorHeap->Heap();
		m_deviceResources->GetCommandList()->SetDescriptorHeaps(1, &descriptorHeap);
	}

	void ProcessDLSSFrameGeneration(RenderTexture& inColor) {
		ResourceTagInfo resourceTagInfos[]{
			CreateResourceTagInfo(kBufferTypeDepth, *m_renderTextures.at(RenderTextureNames::NormalizedDepth)),
			CreateResourceTagInfo(kBufferTypeMotionVectors, *m_renderTextures.at(RenderTextureNames::MotionVectors)),
			CreateResourceTagInfo(kBufferTypeHUDLessColor, inColor, false)
		};
		ignore = m_streamline->Tag(CreateResourceTags(resourceTagInfos));
	}

	void ProcessFSRSuperResolution() {
		const auto commandList = m_deviceResources->GetCommandList();

		reinterpret_cast<XMUINT2&>(m_FSRSettings.RenderSize) = m_renderSize;

		const auto jitter = m_GPUBuffers.Camera->At(0).Jitter;
		const auto farDepth = m_cameraController.GetFarDepth();
		m_FSRSettings.Camera = {
			.Jitter{ -jitter.x, -jitter.y },
			.Near = farDepth == numeric_limits<float>::infinity() ? numeric_limits<float>::max() : farDepth,
			.Far = m_cameraController.GetNearDepth(),
			.VerticalFOV = m_cameraController.GetVerticalFieldOfView()
		};

		m_FSRSettings.ElapsedMilliseconds = static_cast<float>(m_stepTimer.GetElapsedSeconds() * 1000);

		m_FSR->SetConstants(m_FSRSettings);

		m_FSRSettings.Reset = false;

		struct FSRResource {
			FSRResourceType Type;
			RenderTexture& Texture;
			D3D12_RESOURCE_STATES State = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		} resources[]{
			{ FSRResourceType::Depth, *m_renderTextures.at(RenderTextureNames::NormalizedDepth) },
			{ FSRResourceType::MotionVectors, *m_renderTextures.at(RenderTextureNames::MotionVectors) },
			{ FSRResourceType::Color, *m_renderTextures.at(RenderTextureNames::Color) },
			{ FSRResourceType::Output, *m_renderTextures.at(RenderTextureNames::FinalColor), D3D12_RESOURCE_STATE_UNORDERED_ACCESS }
		};

		for (auto& [Type, Texture, State] : resources) {
			const auto state = Texture.GetState();
			Texture.TransitionTo(commandList, State);
			m_FSR->Tag(Type, Texture.GetResource(), State);
			State = state;
		}

		ignore = m_FSR->Dispatch();

		for (const auto& [_, Texture, State] : resources) Texture.TransitionTo(commandList, State);

		const auto descriptorHeap = m_resourceDescriptorHeap->Heap();
		commandList->SetDescriptorHeaps(1, &descriptorHeap);
	}

	void ProcessNIS(RenderTexture& inColor, RenderTexture& outColor) {
		NISOptions NISOptions;
		NISOptions.mode = NISMode::eSharpen;
		NISOptions.sharpness = g_graphicsSettings.PostProcessing.NIS.Sharpness;
		ignore = m_streamline->SetConstants(NISOptions);

		ResourceTagInfo resourceTagInfos[]{
			CreateResourceTagInfo(kBufferTypeScalingInputColor, inColor, false),
			CreateResourceTagInfo(kBufferTypeScalingOutputColor, outColor, false)
		};
		ignore = m_streamline->EvaluateFeature(kFeatureNIS, CreateResourceTags(resourceTagInfos));

		const auto descriptorHeap = m_resourceDescriptorHeap->Heap();
		m_deviceResources->GetCommandList()->SetDescriptorHeaps(1, &descriptorHeap);
	}

	void ProcessChromaticAberration(RenderTexture& inColor, RenderTexture& outColor) {
		const auto commandList = m_deviceResources->GetCommandList();

		const auto outputSize = GetOutputSize();
		m_chromaticAberration->Constants = { .RenderSize = XMUINT2(outputSize.cx, outputSize.cy) };

		const ScopedBarrier scopedBarrier(commandList, { CD3DX12_RESOURCE_BARRIER::Transition(inColor.GetResource(), inColor.GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE) });

		m_chromaticAberration->GPUDescriptors = {
			.InColor = m_resourceDescriptorHeap->GetGpuHandle(inColor.GetSrvDescriptorHeapIndex()),
			.OutColor = m_resourceDescriptorHeap->GetGpuHandle(outColor.GetUavDescriptorHeapIndex())
		};

		m_chromaticAberration->Process(commandList);
	}

	void ProcessBloom(RenderTexture& inColor, RenderTexture& outColor) {
		const auto commandList = m_deviceResources->GetCommandList();

		const auto& bloomSettings = g_graphicsSettings.PostProcessing.Bloom;

		const auto& [Extraction, Blur, Combination] = m_bloom;

		auto& blur1 = *m_renderTextures.at(RenderTextureNames::Blur1), & blur2 = *m_renderTextures.at(RenderTextureNames::Blur2);

		const auto inColorState = inColor.GetState(), blur1State = blur1.GetState(), blur2State = blur2.GetState();

		const auto
			inColorSRV = m_resourceDescriptorHeap->GetGpuHandle(inColor.GetSrvDescriptorHeapIndex()),
			blur1SRV = m_resourceDescriptorHeap->GetGpuHandle(blur1.GetSrvDescriptorHeapIndex()),
			blur2SRV = m_resourceDescriptorHeap->GetGpuHandle(blur2.GetSrvDescriptorHeapIndex());

		const auto
			blur1RTV = m_renderDescriptorHeap->GetCpuHandle(blur1.GetRtvDescriptorHeapIndex()),
			blur2RTV = m_renderDescriptorHeap->GetCpuHandle(blur2.GetRtvDescriptorHeapIndex());

		inColor.TransitionTo(commandList, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		Extraction->SetSourceTexture(inColorSRV, inColor.GetResource());
		Extraction->SetBloomExtractParameter(bloomSettings.Threshold);

		commandList->OMSetRenderTargets(1, &blur1RTV, FALSE, nullptr);

		const auto viewPort = m_deviceResources->GetScreenViewport();

		auto halfViewPort = viewPort;
		halfViewPort.Height /= 2;
		halfViewPort.Width /= 2;
		commandList->RSSetViewports(1, &halfViewPort);

		Extraction->Process(commandList);

		blur1.TransitionTo(commandList, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		Blur->SetSourceTexture(blur1SRV, blur1.GetResource());
		Blur->SetBloomBlurParameters(true, bloomSettings.BlurSize, 1);

		commandList->OMSetRenderTargets(1, &blur2RTV, FALSE, nullptr);

		Blur->Process(commandList);

		blur1.TransitionTo(commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);
		blur2.TransitionTo(commandList, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		Blur->SetSourceTexture(blur2SRV, blur2.GetResource());
		Blur->SetBloomBlurParameters(false, bloomSettings.BlurSize, 1);

		commandList->OMSetRenderTargets(1, &blur1RTV, FALSE, nullptr);

		Blur->Process(commandList);

		blur1.TransitionTo(commandList, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		Combination->SetSourceTexture(inColorSRV);
		Combination->SetSourceTexture2(blur1SRV);
		Combination->SetBloomCombineParameters(1.25f, 1, 1, 1);

		auto renderTargetView = m_renderDescriptorHeap->GetCpuHandle(outColor.GetRtvDescriptorHeapIndex());
		commandList->OMSetRenderTargets(1, &renderTargetView, FALSE, nullptr);

		commandList->RSSetViewports(1, &viewPort);

		Combination->Process(commandList);

		inColor.TransitionTo(commandList, inColorState);
		blur1.TransitionTo(commandList, blur1State);
		blur2.TransitionTo(commandList, blur2State);

		renderTargetView = m_deviceResources->GetRenderTargetView();
		commandList->OMSetRenderTargets(1, &renderTargetView, FALSE, nullptr);
	}

	void ProcessToneMapping(RenderTexture& inColor) {
		const auto commandList = m_deviceResources->GetCommandList();

		const ScopedBarrier scopedBarrier(commandList, { CD3DX12_RESOURCE_BARRIER::Transition(inColor.GetResource(), inColor.GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE) });

		const auto toneMappingSettings = g_graphicsSettings.PostProcessing.ToneMapping;
		auto& toneMapping = *m_toneMapping[toneMappingSettings.Operator];
		toneMapping.SetHDRSourceTexture(m_resourceDescriptorHeap->GetGpuHandle(inColor.GetSrvDescriptorHeapIndex()));
		toneMapping.SetExposure(toneMappingSettings.Exposure);
		toneMapping.Process(commandList);
	}

	void ProcessAlphaBlending(RenderTexture& inColor) {
		const auto commandList = m_deviceResources->GetCommandList();

		const ScopedBarrier scopedBarrier(commandList, { CD3DX12_RESOURCE_BARRIER::Transition(inColor.GetResource(), inColor.GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE) });

		m_alphaBlending->SetViewport(m_deviceResources->GetScreenViewport());
		m_alphaBlending->Begin(commandList);
		m_alphaBlending->Draw(m_resourceDescriptorHeap->GetGpuHandle(inColor.GetSrvDescriptorHeapIndex()), GetTextureSize(inColor.GetResource()), XMFLOAT2());
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
					auto isEnabled = m_deviceResources->IsVSyncEnabled();
					if (const ImGuiEx::ScopedEnablement scopedEnablement(m_deviceResources->GetDeviceOptions() & DeviceResources::c_AllowTearing);
						ImGui::Checkbox("V-Sync", &isEnabled) && m_deviceResources->EnableVSync(isEnabled)) g_graphicsSettings.IsVSyncEnabled = isEnabled;
				}

				{
					ReflexState state;
					ignore = slReflexGetState(state);
					const auto isAvailable = m_streamline->IsFeatureAvailable(kFeatureReflex) && state.lowLatencyAvailable;
					if (const ImGuiEx::ScopedEnablement scopedEnablement(isAvailable);
						ImGui::BeginCombo("NVIDIA Reflex", ToString(isAvailable ? g_graphicsSettings.ReflexMode : ReflexMode::eOff))) {
						for (const auto ReflexMode : { ReflexMode::eOff, ReflexMode::eLowLatency, ReflexMode::eLowLatencyWithBoost }) {
							const auto isSelected = g_graphicsSettings.ReflexMode == ReflexMode;

							if (ImGui::Selectable(ToString(ReflexMode), isSelected)) {
								g_graphicsSettings.ReflexMode = ReflexMode;

								SetReflexOptions();
								SetFrameGenerationOptions();
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

					isChanged |= ImGui::SliderInt("Max Number of Bounces", reinterpret_cast<int*>(&raytracingSettings.MaxNumberOfBounces), 0, raytracingSettings.MaxMaxNumberOfBounces, "%d", ImGuiSliderFlags_AlwaysClamp);

					isChanged |= ImGui::SliderInt("Samples per Pixel", reinterpret_cast<int*>(&raytracingSettings.SamplesPerPixel), 1, raytracingSettings.MaxSamplesPerPixel, "%d", ImGuiSliderFlags_AlwaysClamp);

					if (ImGui::TreeNodeEx("NVIDIA RTX Direct Illumination", ImGuiTreeNodeFlags_DefaultOpen)) {
						auto& RTXDISettings = raytracingSettings.RTXDI;

						{
							const ImGuiEx::ScopedID scopedID("Enable NVIDIA RTX Direct Illumination");

							isChanged |= ImGui::Checkbox("Enable", &RTXDISettings.IsEnabled);
						}

						if (RTXDISettings.IsEnabled) {
							isChanged |= ImGui::SliderInt("Local Light Samples", reinterpret_cast<int*>(&RTXDISettings.LocalLightSamples), 1, RTXDISettings.MaxLocalLightSamples, "%d", ImGuiSliderFlags_AlwaysClamp);

							isChanged |= ImGui::SliderInt("BRDF Samples", reinterpret_cast<int*>(&RTXDISettings.BRDFSamples), 0, RTXDISettings.MaxBRDFSamples, "%d", ImGuiSliderFlags_AlwaysClamp);

							isChanged |= ImGui::SliderInt("Spatio-Temporal Samples", reinterpret_cast<int*>(&RTXDISettings.SpatioTemporalSamples), 0, RTXDISettings.MaxSpatioTemporalSamples, "%d", ImGuiSliderFlags_AlwaysClamp);
						}

						ImGui::TreePop();
					}

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

						bool isChanged = false;

						if (ImGui::BeginCombo("Upscaler", ToString(superResolutionSettings.Upscaler))) {
							for (const auto Upscaler : { Upscaler::None, Upscaler::DLSS, Upscaler::FSR }) {
								const auto IsSelectable = [&] {
									return Upscaler == Upscaler::None
										|| (Upscaler == Upscaler::DLSS && m_streamline->IsFeatureAvailable(kFeatureDLSS))
										|| (Upscaler == Upscaler::FSR && m_FSR->IsAvailable());
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

							SetFrameGenerationOptions();
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

						if (bloomSettings.IsEnabled) {
							ImGui::SliderFloat("Threshold", &bloomSettings.Threshold, 0, 1, "%.2f", ImGuiSliderFlags_AlwaysClamp);

							ImGui::SliderFloat("Blur size", &bloomSettings.BlurSize, 1, bloomSettings.MaxBlurSize, "%.2f", ImGuiSliderFlags_AlwaysClamp);
						}

						ImGui::TreePop();
					}

					if (ImGui::TreeNodeEx("Tone Mapping", ImGuiTreeNodeFlags_DefaultOpen)) {
						auto& toneMappingSettings = postProcessingSetttings.ToneMapping;

						if (ImGui::BeginCombo("Operator", ToString(toneMappingSettings.Operator))) {
							for (const auto Operator : { ToneMapPostProcess::None, ToneMapPostProcess::Saturate, ToneMapPostProcess::Reinhard, ToneMapPostProcess::ACESFilmic }) {
								const auto isSelected = toneMappingSettings.Operator == Operator;

								if (ImGui::Selectable(ToString(Operator), isSelected)) toneMappingSettings.Operator = Operator;

								if (isSelected) ImGui::SetItemDefaultFocus();
							}

							ImGui::EndCombo();
						}

						if (toneMappingSettings.Operator != ToneMapPostProcess::None) {
							ImGui::SliderFloat("Exposure", &toneMappingSettings.Exposure, toneMappingSettings.MinExposure, toneMappingSettings.MaxExposure, "%.2f", ImGuiSliderFlags_AlwaysClamp);
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
