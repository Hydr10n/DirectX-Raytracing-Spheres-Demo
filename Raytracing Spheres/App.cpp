module;

#include "pch.h"

#include "directxtk12/GraphicsMemory.h"

#include "directxtk12/GamePad.h"
#include "directxtk12/Keyboard.h"
#include "directxtk12/Mouse.h"

#include "directxtk12/DirectXHelpers.h"

#include "directxtk12/ResourceUploadBatch.h"

#include "directxtk12/CommonStates.h"

#include "directxtk12/SpriteBatch.h"

#include "directxtk12/PostProcess.h"

#include "sl_helpers.h"

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
import DeviceResources;
import DirectX.BufferHelpers;
import DirectX.CommandList;
import DirectX.DescriptorHeap;
import DirectX.PostProcess.DenoisedComposition;
import DirectX.PostProcess.PreDLSS;
import DirectX.PostProcess.TemporalAntiAliasing;
import DirectX.RaytracingHelpers;
import HaltonSamplePattern;
import Model;
import MyScene;
import NRD;
import RaytracingShaderData;
import RenderTexture;
import Scene;
import SharedData;
import StepTimer;
import Texture;
import ThreadHelpers;

using namespace DirectX;
using namespace DirectX::BufferHelpers;
using namespace DirectX::PostProcess;
using namespace DirectX::RaytracingHelpers;
using namespace DirectX::SimpleMath;
using namespace DX;
using namespace Microsoft::WRL;
using namespace nrd;
using namespace physx;
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
	Impl(const shared_ptr<WindowModeHelper>& windowModeHelper) noexcept(false) : m_windowModeHelper(windowModeHelper) {
		CheckSettings();

		{
			ImGui::CreateContext();

			ImGui::StyleColorsDark();

			auto& IO = ImGui::GetIO();

			IO.IniFilename = IO.LogFilename = nullptr;

			IO.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
			IO.BackendFlags |= ImGuiBackendFlags_HasGamepad;

			ImGui_ImplWin32_Init(m_windowModeHelper->hWnd);

			m_UIStates.IsVisible = g_UISettings.ShowOnStartup;
		}

		{
			m_deviceResources->RegisterDeviceNotify(this);

			m_deviceResources->SetWindow(windowModeHelper->hWnd, windowModeHelper->GetResolution());

			m_deviceResources->EnableVSync(g_graphicsSettings.IsVSyncEnabled);

			m_deviceResources->CreateDeviceResources();
			CreateDeviceDependentResources();

			m_deviceResources->CreateWindowSizeDependentResources();
			CreateWindowSizeDependentResources();
		}

		m_inputDevices.Mouse->SetWindow(windowModeHelper->hWnd);

		windowModeHelper->SetFullscreenResolutionHandledByWindow(false);
	}

	~Impl() {
		{
			if (ImGui::GetIO().BackendRendererUserData != nullptr) ImGui_ImplDX12_Shutdown();

			ImGui_ImplWin32_Shutdown();

			ImGui::DestroyContext();
		}

		m_deviceResources->WaitForGpu();
	}

	SIZE GetOutputSize() const noexcept { return m_deviceResources->GetOutputSize(); }

	void Tick() {
		{
			const scoped_lock lock(m_exceptionMutex);

			if (m_exception) rethrow_exception(m_exception);
		}

		if (const auto pFuture = m_futures.find(FutureNames::Scene); pFuture != cend(m_futures) && pFuture->second.wait_for(0s) == future_status::ready) m_futures.erase(pFuture);

		m_stepTimer.Tick([&] { if (!m_futures.contains(FutureNames::Scene) && m_scene) Update(); });

		Render();

		m_inputDevices.Mouse->EndOfInputFrame();

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

		m_scene.reset();

		m_renderTextures = {};

		m_GPUBuffers = {};

		m_topLevelAccelerationStructure.reset();
		m_bottomLevelAccelerationStructures = {};

		m_alphaBlending.reset();

		m_bloom = {};

		for (auto& toneMapping : m_toneMapping) toneMapping.reset();

		m_preDLSS.reset();

		m_temporalAntiAliasing.reset();

		m_denoisedComposition.reset();
		m_NRD.reset();

		m_streamline.reset();

		m_pipelineState.Reset();

		m_rootSignature.Reset();

		m_renderDescriptorHeap.reset();
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

	struct FutureNames {
		MAKE_NAME(Scene);
	};
	map<string, future<void>, less<>> m_futures;

	mutex m_exceptionMutex;
	exception_ptr m_exception;

	struct ResourceDescriptorHeapIndex {
		enum {
			InGraphicsSettings,
			InCamera,
			InSceneData,
			InInstanceData,
			InObjectResourceDescriptorHeapIndices, InObjectData,
			InHistoryOutput, OutHistoryOutput,
			InOutput, Output,
			InFinalOutput, OutFinalOutput,
			InDepth, OutDepth,
			InMotionVectors2D, OutMotionVectors2D,
			InMotionVectors3D, OutMotionVectors3D,
			InBaseColorMetalness, OutBaseColorMetalness,
			InNormalRoughness, OutNormalRoughness,
			OutNoisyDiffuse, OutNoisySpecular,
			InDenoisedDiffuse, OutDenoisedDiffuse,
			InDenoisedSpecular, OutDenoisedSpecular,
			InValidation, OutValidation,
			InBlur1, InBlur2,
			InFont,
			Reserve,
			Count = 1 << 16
		};
	};
	unique_ptr<DescriptorHeapEx> m_resourceDescriptorHeap;

	struct RenderDescriptorHeapIndex {
		enum {
			Output,
			FinalOutput,
			Blur1, Blur2,
			Count
		};
	};
	unique_ptr<DescriptorHeap> m_renderDescriptorHeap;

	ComPtr<ID3D12RootSignature> m_rootSignature;

	ComPtr<ID3D12PipelineState> m_pipelineState;

	static constexpr UINT MaxTraceRecursionDepth = 32, MaxSamplesPerPixel = 16;

	Constants m_slConstants = [] {
		Constants constants;
		constants.cameraPinholeOffset = { 0, 0 };
		constants.depthInverted = Boolean::eFalse;
		constants.cameraMotionIncluded = Boolean::eTrue;
		constants.motionVectors3D = Boolean::eFalse;
		constants.reset = Boolean::eFalse;
		return constants;
	}();
	unique_ptr<Streamline> m_streamline;

	CommonSettings m_NRDCommonSettings{ .isBaseColorMetalnessAvailable = true };
	ReblurSettings m_NRDReblurSettings{ .hitDistanceReconstructionMode = HitDistanceReconstructionMode::AREA_5X5, .enableAntiFirefly = true };
	unique_ptr<NRD> m_NRD;
	unique_ptr<DenoisedComposition> m_denoisedComposition;

	unique_ptr<TemporalAntiAliasing> m_temporalAntiAliasing;

	DLSSOptimalSettings m_DLSSOptimalSettings;
	DLSSOptions m_DLSSOptions = [] {
		DLSSOptions DLSSOptions;
		DLSSOptions.colorBuffersHDR = Boolean::eFalse;
		return DLSSOptions;
	}();
	unique_ptr<PreDLSS> m_preDLSS;

	static constexpr float BloomMaxBlurSize = 5;
	struct {
		unique_ptr<BasicPostProcess> Extraction, Blur;
		unique_ptr<DualPostProcess> Combination;
	} m_bloom;

	unique_ptr<ToneMapPostProcess> m_toneMapping[ToneMapPostProcess::Operator_Max];

	unique_ptr<SpriteBatch> m_alphaBlending;

	unordered_map<shared_ptr<Mesh>, shared_ptr<BottomLevelAccelerationStructure>> m_bottomLevelAccelerationStructures;
	unique_ptr<TopLevelAccelerationStructure> m_topLevelAccelerationStructure;

	struct {
		unique_ptr<ConstantBuffer<GlobalResourceDescriptorHeapIndices>> GlobalResourceDescriptorHeapIndices;
		unique_ptr<ConstantBuffer<GraphicsSettings>> GraphicsSettings;
		unique_ptr<ConstantBuffer<Camera>> Camera;
		unique_ptr<ConstantBuffer<SceneData>> SceneData;
		unique_ptr<StructuredBuffer<InstanceData>> InstanceData;
		unique_ptr<StructuredBuffer<ObjectResourceDescriptorHeapIndices>> ObjectResourceDescriptorHeapIndices;
		unique_ptr<StructuredBuffer<ObjectData>> ObjectData;
	} m_GPUBuffers;

	struct RenderTextureNames {
		MAKE_NAME(HistoryOutput);
		MAKE_NAME(Output);
		MAKE_NAME(FinalOutput);
		MAKE_NAME(Depth);
		MAKE_NAME(MotionVectors2D);
		MAKE_NAME(MotionVectors3D);
		MAKE_NAME(BaseColorMetalness);
		MAKE_NAME(NormalRoughness);
		MAKE_NAME(NoisyDiffuse);
		MAKE_NAME(NoisySpecular);
		MAKE_NAME(DenoisedDiffuse);
		MAKE_NAME(DenoisedSpecular);
		MAKE_NAME(Validation);
		MAKE_NAME(Blur1);
		MAKE_NAME(Blur2);
	};
	map<string, shared_ptr<RenderTexture>, less<>> m_renderTextures;

	static constexpr float
		CameraMinVerticalFieldOfView = 30, CameraMaxVerticalFieldOfView = 120,
		CameraMaxMovementSpeed = 100, CameraMaxRotationSpeed = 2;
	CameraController m_cameraController;

	unique_ptr<Scene> m_scene;

	HaltonSamplePattern m_haltonSamplePattern;

	struct { bool IsVisible, HasFocus = true, IsSettingsWindowVisible; } m_UIStates{};

	void CreateDeviceDependentResources() {
		const auto device = m_deviceResources->GetD3DDevice();

		m_graphicsMemory = make_unique<GraphicsMemory>(device);

		CreateDescriptorHeaps();

		CreateRootSignatures();

		CreatePipelineStates();

		CreateConstantBuffers();

		LoadScene();
	}

	void CreateWindowSizeDependentResources() {
		const auto device = m_deviceResources->GetD3DDevice();

		const auto outputSize = GetOutputSize();

		{
			const auto CreateTexture = [&](DXGI_FORMAT format, SIZE size, LPCSTR textureName, UINT srvDescriptorHeapIndex = ~0u, UINT uavDescriptorHeapIndex = ~0u, UINT rtvDescriptorHeapIndex = ~0u) {
				auto& texture = m_renderTextures[textureName];
				texture = make_shared<RenderTexture>(format);
				texture->SetDevice(device, m_resourceDescriptorHeap.get(), srvDescriptorHeapIndex, uavDescriptorHeapIndex, m_renderDescriptorHeap.get(), rtvDescriptorHeapIndex);
				texture->CreateResource(size.cx, size.cy);
			};
			const auto CreateTexture1 = [&](DXGI_FORMAT format, LPCSTR textureName, UINT srvDescriptorHeapIndex = ~0u, UINT uavDescriptorHeapIndex = ~0u, UINT rtvDescriptorHeapIndex = ~0u) {
				return CreateTexture(format, outputSize, textureName, srvDescriptorHeapIndex, uavDescriptorHeapIndex, rtvDescriptorHeapIndex);
			};

			CreateTexture1(DXGI_FORMAT_R16G16B16A16_FLOAT, RenderTextureNames::HistoryOutput, ResourceDescriptorHeapIndex::InHistoryOutput, ResourceDescriptorHeapIndex::OutHistoryOutput);
			CreateTexture1(DXGI_FORMAT_R16G16B16A16_FLOAT, RenderTextureNames::Output, ResourceDescriptorHeapIndex::InOutput, ResourceDescriptorHeapIndex::Output, RenderDescriptorHeapIndex::Output);
			CreateTexture1(DXGI_FORMAT_R16G16B16A16_FLOAT, RenderTextureNames::FinalOutput, ResourceDescriptorHeapIndex::InFinalOutput, ResourceDescriptorHeapIndex::OutFinalOutput, RenderDescriptorHeapIndex::FinalOutput);

			CreateTexture1(DXGI_FORMAT_R32_FLOAT, RenderTextureNames::Depth, ResourceDescriptorHeapIndex::InDepth, ResourceDescriptorHeapIndex::OutDepth, ~0u);
			CreateTexture1(DXGI_FORMAT_R16G16B16A16_FLOAT, RenderTextureNames::MotionVectors3D, ResourceDescriptorHeapIndex::InMotionVectors3D, ResourceDescriptorHeapIndex::OutMotionVectors3D);

			if (m_NRD = make_unique<NRD>(device, m_deviceResources->GetCommandQueue(), m_deviceResources->GetCommandList(), m_deviceResources->GetBackBufferCount(), initializer_list{ Method::REBLUR_DIFFUSE_SPECULAR }, outputSize);
				m_NRD->IsAvailable()) {
				CreateTexture1(DXGI_FORMAT_R8G8B8A8_UNORM, RenderTextureNames::BaseColorMetalness, ResourceDescriptorHeapIndex::InBaseColorMetalness, ResourceDescriptorHeapIndex::OutBaseColorMetalness);
				CreateTexture1(NRD::ToDXGIFormat(GetLibraryDesc().normalEncoding), RenderTextureNames::NormalRoughness, ResourceDescriptorHeapIndex::InNormalRoughness, ResourceDescriptorHeapIndex::OutNormalRoughness);
				CreateTexture1(DXGI_FORMAT_R16G16B16A16_FLOAT, RenderTextureNames::NoisyDiffuse, ~0u, ResourceDescriptorHeapIndex::OutNoisyDiffuse);
				CreateTexture1(DXGI_FORMAT_R16G16B16A16_FLOAT, RenderTextureNames::NoisySpecular, ~0u, ResourceDescriptorHeapIndex::OutNoisySpecular);
				CreateTexture1(DXGI_FORMAT_R16G16B16A16_FLOAT, RenderTextureNames::DenoisedDiffuse, ResourceDescriptorHeapIndex::InDenoisedDiffuse, ResourceDescriptorHeapIndex::OutDenoisedDiffuse);
				CreateTexture1(DXGI_FORMAT_R16G16B16A16_FLOAT, RenderTextureNames::DenoisedSpecular, ResourceDescriptorHeapIndex::InDenoisedSpecular, ResourceDescriptorHeapIndex::OutDenoisedSpecular);
				CreateTexture1(DXGI_FORMAT_R8G8B8A8_UNORM, RenderTextureNames::Validation, ResourceDescriptorHeapIndex::InValidation, ResourceDescriptorHeapIndex::OutValidation);
			}

			if (m_streamline->IsFeatureAvailable(kFeatureDLSS)) {
				CreateTexture1(DXGI_FORMAT_R16G16_FLOAT, RenderTextureNames::MotionVectors2D, ResourceDescriptorHeapIndex::InMotionVectors2D, ResourceDescriptorHeapIndex::OutMotionVectors2D);

				m_DLSSOptions.outputWidth = outputSize.cx;
				m_DLSSOptions.outputHeight = outputSize.cy;
				SetDLSSOptimalSettings();
			}

			{
				const SIZE size{ outputSize.cx / 2, outputSize.cy / 2 };
				CreateTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, size, RenderTextureNames::Blur1, ResourceDescriptorHeapIndex::InBlur1, ~0u, RenderDescriptorHeapIndex::Blur1);
				CreateTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, size, RenderTextureNames::Blur2, ResourceDescriptorHeapIndex::InBlur2, ~0u, RenderDescriptorHeapIndex::Blur2);
			}
		}

		m_cameraController.SetLens(XMConvertToRadians(g_graphicsSettings.Camera.VerticalFieldOfView), static_cast<float>(outputSize.cx) / static_cast<float>(outputSize.cy));

		m_haltonSamplePattern.Reset();

		{
			const auto& IO = ImGui::GetIO();

			if (IO.BackendRendererUserData != nullptr) ImGui_ImplDX12_Shutdown();
			ImGui_ImplDX12_Init(device, m_deviceResources->GetBackBufferCount(), m_deviceResources->GetBackBufferFormat(), m_resourceDescriptorHeap->Heap(), m_resourceDescriptorHeap->GetCpuHandle(ResourceDescriptorHeapIndex::InFont), m_resourceDescriptorHeap->GetGpuHandle(ResourceDescriptorHeapIndex::InFont));

			IO.Fonts->Clear();
			IO.Fonts->AddFontFromFileTTF(R"(C:\Windows\Fonts\segoeui.ttf)", static_cast<float>(outputSize.cy) * 0.022f);
		}
	}

	void Update() {
		PIXBeginEvent(PIX_COLOR_DEFAULT, L"Update");

		{
			auto& camera = m_GPUBuffers.Camera->GetData();

			camera.PreviousWorldToView = m_cameraController.GetWorldToView();
			camera.PreviousViewToProjection = m_cameraController.GetViewToProjection();
			camera.PreviousWorldToProjection = m_cameraController.GetWorldToProjection();
			camera.PreviousViewToWorld = m_cameraController.GetViewToWorld();

			ProcessInput();

			camera.Position = m_cameraController.GetPosition();
			camera.RightDirection = m_cameraController.GetRightDirection();
			camera.UpDirection = m_cameraController.GetUpDirection();
			camera.ForwardDirection = m_cameraController.GetForwardDirection();
			camera.NearZ = m_cameraController.GetNearZ();
			camera.FarZ = m_cameraController.GetFarZ();
			camera.PixelJitter = g_graphicsSettings.Camera.IsJitterEnabled ? m_haltonSamplePattern.GetNext() : XMFLOAT2();
		}

		UpdateGraphicsSettings();

		UpdateScene();

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

		const auto outputSize = m_deviceResources->GetOutputSize();
		m_renderSize = IsDLSSSuperResolutionEnabled() ? XMUINT2(m_DLSSOptimalSettings.optimalRenderWidth, m_DLSSOptimalSettings.optimalRenderHeight) : XMUINT2(outputSize.cx, outputSize.cy);

		const auto descriptorHeap = m_resourceDescriptorHeap->Heap();
		commandList->SetDescriptorHeaps(1, &descriptorHeap);

		if (!m_futures.contains(FutureNames::Scene) && m_scene) {
			if (!m_scene->IsWorldStatic()) CreateAccelerationStructures(commandList, true);
			DispatchRays();

			PostProcessGraphics();
		}

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
			auto& cameraSettings = g_graphicsSettings.Camera;
			cameraSettings.VerticalFieldOfView = clamp(cameraSettings.VerticalFieldOfView, CameraMinVerticalFieldOfView, CameraMaxVerticalFieldOfView);
		}

		{
			auto& raytracingSettings = g_graphicsSettings.Raytracing;
			raytracingSettings.MaxTraceRecursionDepth = clamp(raytracingSettings.MaxTraceRecursionDepth, 1u, MaxTraceRecursionDepth);
			raytracingSettings.SamplesPerPixel = clamp(raytracingSettings.SamplesPerPixel, 1u, MaxSamplesPerPixel);
		}

		{
			auto& postProcessingSettings = g_graphicsSettings.PostProcessing;

			postProcessingSettings.NRD.SplitScreen = clamp(postProcessingSettings.NRD.SplitScreen, 0.0f, 1.0f);

			postProcessingSettings.NIS.Sharpness = clamp(postProcessingSettings.NIS.Sharpness, 0.0f, 1.0f);

			{
				auto& bloomSettings = postProcessingSettings.Bloom;
				bloomSettings.Threshold = clamp(bloomSettings.Threshold, 0.0f, 1.0f);
				bloomSettings.BlurSize = clamp(bloomSettings.BlurSize, 1.0f, BloomMaxBlurSize);
			}
		}

		g_UISettings.WindowOpacity = clamp(g_UISettings.WindowOpacity, 0.0f, 1.0f);

		{
			auto& speedSettings = g_controlsSettings.Camera.Speed;
			speedSettings.Movement = clamp(speedSettings.Movement, 0.0f, CameraMaxMovementSpeed);
			speedSettings.Rotation = clamp(speedSettings.Rotation, 0.0f, CameraMaxRotationSpeed);
		}
	}

	static auto Transform(const PxShape& shape) {
		PxVec3 scaling;
		switch (const PxGeometryHolder geometry = shape.getGeometry(); geometry.getType()) {
			case PxGeometryType::eSPHERE: scaling = PxVec3(2 * geometry.sphere().radius); break;
			default: throw;
		}

		PxMat44 world(PxVec4(1, 1, -1, 1));
		world *= PxShapeExt::getGlobalPose(shape, *shape.getActor());
		world.scale(PxVec4(scaling, 1));
		return reinterpret_cast<const Matrix&>(*world.front());
	}

	void LoadScene() {
		m_futures[FutureNames::Scene] = StartDetachedFuture([&] {
			try {
			const auto device = m_deviceResources->GetD3DDevice();
			const auto commandQueue = m_deviceResources->GetCommandQueue();

			{
				UINT descriptorHeapIndex = ResourceDescriptorHeapIndex::Reserve;
				m_scene = make_unique<MyScene>();
				m_scene->Load(MySceneDesc(), device, commandQueue, *m_resourceDescriptorHeap, descriptorHeapIndex);
			}

			ResetCamera();

			{
				CommandList<ID3D12GraphicsCommandList4> commandList(device);
				commandList.Begin();

				CreateAccelerationStructures(commandList.GetNative(), false);

				commandList.End(commandQueue).get();
			}

			{
				auto& globalResourceDescriptorHeapIndices = m_GPUBuffers.GlobalResourceDescriptorHeapIndices->GetData();
				globalResourceDescriptorHeapIndices.InEnvironmentLightCubeMap = m_scene->EnvironmentLightCubeMap.Texture.DescriptorHeapIndices.SRV;
				globalResourceDescriptorHeapIndices.InEnvironmentCubeMap = m_scene->EnvironmentCubeMap.Texture.DescriptorHeapIndices.SRV;
			}

			CreateStructuredBuffers();
		}
		catch (...) {
			const scoped_lock lock(m_exceptionMutex);

			if (!m_exception) m_exception = current_exception();
		}
			});
	}

	void CreateDescriptorHeaps() {
		const auto device = m_deviceResources->GetD3DDevice();

		m_resourceDescriptorHeap = make_unique<DescriptorHeapEx>(device, ResourceDescriptorHeapIndex::Count, ResourceDescriptorHeapIndex::Reserve);

		m_renderDescriptorHeap = make_unique<DescriptorHeap>(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, RenderDescriptorHeapIndex::Count);
	}

	void CreateRootSignatures() {
		const auto device = m_deviceResources->GetD3DDevice();

		ThrowIfFailed(device->CreateRootSignature(0, g_pRaytracing, size(g_pRaytracing), IID_PPV_ARGS(&m_rootSignature)));
	}

	void CreatePipelineStates() {
		const auto device = m_deviceResources->GetD3DDevice();

		{
			const CD3DX12_SHADER_BYTECODE shaderByteCode(g_pRaytracing, size(g_pRaytracing));
			const D3D12_COMPUTE_PIPELINE_STATE_DESC computePipelineStateDesc{ .pRootSignature = m_rootSignature.Get(), .CS = shaderByteCode };
			ThrowIfFailed(device->CreateComputePipelineState(&computePipelineStateDesc, IID_PPV_ARGS(&m_pipelineState)));
		}

		CreatePostProcess();
	}

	void CreatePostProcess() {
		const auto device = m_deviceResources->GetD3DDevice();
		const auto commandQueue = m_deviceResources->GetCommandQueue();
		const auto commandList = m_deviceResources->GetCommandList();

		{
			slSetD3DDevice(device);

			DXGI_ADAPTER_DESC adapterDesc;
			ThrowIfFailed(m_deviceResources->GetAdapter()->GetDesc(&adapterDesc));
			m_streamline = make_unique<Streamline>(0, adapterDesc.AdapterLuid, commandList);
		}

		m_denoisedComposition = make_unique<DenoisedComposition>(device);

		m_temporalAntiAliasing = make_unique<TemporalAntiAliasing>(device);

		m_preDLSS = make_unique<PreDLSS>(device);

		{
			const RenderTargetState renderTargetState(DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_UNKNOWN);

			m_bloom = {
				.Extraction = make_unique<BasicPostProcess>(device, renderTargetState, BasicPostProcess::BloomExtract),
				.Blur = make_unique<BasicPostProcess>(device, renderTargetState, BasicPostProcess::BloomBlur),
				.Combination = make_unique<DualPostProcess>(device, renderTargetState, DualPostProcess::BloomCombine),
			};
		}

		{

			for (const auto toneMappingOperator : { ToneMapPostProcess::None, ToneMapPostProcess::Saturate, ToneMapPostProcess::Reinhard, ToneMapPostProcess::ACESFilmic }) {
				m_toneMapping[toneMappingOperator] = make_unique<ToneMapPostProcess>(device, RenderTargetState(m_deviceResources->GetBackBufferFormat(), DXGI_FORMAT_UNKNOWN), toneMappingOperator, toneMappingOperator == ToneMapPostProcess::None ? ToneMapPostProcess::Linear : ToneMapPostProcess::SRGB);
			}
		}

		{
			ResourceUploadBatch resourceUploadBatch(device);
			resourceUploadBatch.Begin();

			m_alphaBlending = make_unique<SpriteBatch>(device, resourceUploadBatch, SpriteBatchPipelineStateDescription(RenderTargetState(m_deviceResources->GetBackBufferFormat(), m_deviceResources->GetDepthBufferFormat()), &CommonStates::NonPremultiplied));

			resourceUploadBatch.End(commandQueue).get();
		}
	}

	void CreateAccelerationStructures(ID3D12GraphicsCommandList4* pCommandList, bool updateOnly) {
		const auto device = m_deviceResources->GetD3DDevice();

		if (!updateOnly) {
			m_bottomLevelAccelerationStructures.clear();

			for (const auto& Mesh : m_scene->Meshes | views::values) {
				if (const auto [first, second] = m_bottomLevelAccelerationStructures.try_emplace(Mesh); second) {
					first->second = make_shared<BottomLevelAccelerationStructure>(device, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE);
					first->second->Build(pCommandList, initializer_list{ CreateGeometryDesc<Mesh::VertexType, Mesh::IndexType>(Mesh->Vertices.Get(), Mesh->Indices.Get()) }, false);
				}
			}

			m_topLevelAccelerationStructure = make_unique<TopLevelAccelerationStructure>(device, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE);
		}
		vector<D3D12_RAYTRACING_INSTANCE_DESC> instanceDescs;
		instanceDescs.reserve(m_topLevelAccelerationStructure->GetDescCount());
		for (UINT objectIndex = 0; const auto & renderObject : m_scene->RenderObjects) {
			D3D12_RAYTRACING_INSTANCE_DESC instanceDesc;
			XMStoreFloat3x4(reinterpret_cast<XMFLOAT3X4*>(instanceDesc.Transform), Transform(*renderObject.Shape));
			instanceDesc.InstanceID = objectIndex;
			instanceDesc.InstanceMask = renderObject.IsVisible ? ~0u : 0;
			instanceDesc.InstanceContributionToHitGroupIndex = 0;
			instanceDesc.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE | D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_FRONT_COUNTERCLOCKWISE;
			instanceDesc.AccelerationStructure = m_bottomLevelAccelerationStructures.at(renderObject.Mesh)->GetBuffer()->GetGPUVirtualAddress();
			instanceDescs.emplace_back(instanceDesc);
			objectIndex++;
		}
		m_topLevelAccelerationStructure->Build(pCommandList, instanceDescs, updateOnly);
	}

	void CreateConstantBuffers() {
		const auto CreateBuffer = [&]<typename T>(unique_ptr<T>&uploadBuffer, const auto & data, UINT descriptorHeapIndex = ~0u) {
			uploadBuffer = make_unique<T>(m_deviceResources->GetD3DDevice());
			uploadBuffer->GetData() = data;
			if (descriptorHeapIndex != ~0u) uploadBuffer->CreateConstantBufferView(m_resourceDescriptorHeap->GetCpuHandle(descriptorHeapIndex));
		};
		CreateBuffer(
			m_GPUBuffers.GlobalResourceDescriptorHeapIndices,
			GlobalResourceDescriptorHeapIndices{
				.InGraphicsSettings = ResourceDescriptorHeapIndex::InGraphicsSettings,
				.InCamera = ResourceDescriptorHeapIndex::InCamera,
				.InSceneData = ResourceDescriptorHeapIndex::InSceneData,
				.InInstanceData = ResourceDescriptorHeapIndex::InInstanceData,
				.InObjectResourceDescriptorHeapIndices = ResourceDescriptorHeapIndex::InObjectResourceDescriptorHeapIndices,
				.InObjectData = ResourceDescriptorHeapIndex::InObjectData,
				.Output = ResourceDescriptorHeapIndex::Output,
				.OutDepth = ResourceDescriptorHeapIndex::OutDepth,
				.OutMotionVectors3D = ResourceDescriptorHeapIndex::OutMotionVectors3D,
				.OutBaseColorMetalness = ResourceDescriptorHeapIndex::OutBaseColorMetalness,
				.OutNormalRoughness = ResourceDescriptorHeapIndex::OutNormalRoughness,
				.OutNoisyDiffuse = ResourceDescriptorHeapIndex::OutNoisyDiffuse,
				.OutNoisySpecular = ResourceDescriptorHeapIndex::OutNoisySpecular
			}
		);
		CreateBuffer(m_GPUBuffers.GraphicsSettings, GraphicsSettings(), ResourceDescriptorHeapIndex::InGraphicsSettings);
		CreateBuffer(m_GPUBuffers.Camera, Camera(), ResourceDescriptorHeapIndex::InCamera);
		CreateBuffer(m_GPUBuffers.SceneData, SceneData(), ResourceDescriptorHeapIndex::InSceneData);
	}

	void CreateStructuredBuffers() {
		if (const UINT objectCount = static_cast<UINT>(size(m_scene->RenderObjects))) {
			const auto CreateBuffer = [&]<typename T>(unique_ptr<T>&uploadBuffer, UINT count, UINT descriptorHeapIndex) {
				uploadBuffer = make_unique<T>(m_deviceResources->GetD3DDevice(), count);
				uploadBuffer->CreateShaderResourceView(m_resourceDescriptorHeap->GetCpuHandle(descriptorHeapIndex));
			};
			CreateBuffer(m_GPUBuffers.InstanceData, m_topLevelAccelerationStructure->GetDescCount(), ResourceDescriptorHeapIndex::InInstanceData);
			CreateBuffer(m_GPUBuffers.ObjectResourceDescriptorHeapIndices, objectCount, ResourceDescriptorHeapIndex::InObjectResourceDescriptorHeapIndices);
			CreateBuffer(m_GPUBuffers.ObjectData, objectCount, ResourceDescriptorHeapIndex::InObjectData);

			for (UINT instanceIndex = 0, objectIndex = 0, textureTransformIndex = 0; const auto & renderObject : m_scene->RenderObjects) {
				m_GPUBuffers.InstanceData->GetData(instanceIndex++).PreviousObjectToWorld = Transform(*renderObject.Shape);

				auto& objectData = m_GPUBuffers.ObjectData->GetData(objectIndex);

				objectData.Material = renderObject.Material;

				objectData.TextureTransform = renderObject.TextureTransform();

				auto& objectResourceDescriptorHeapIndices = m_GPUBuffers.ObjectResourceDescriptorHeapIndices->GetData(objectIndex);
				objectResourceDescriptorHeapIndices = {
					.Mesh{
						.Vertices = renderObject.Mesh->DescriptorHeapIndices.Vertices,
						.Indices = renderObject.Mesh->DescriptorHeapIndices.Indices
					}
				};

				for (const auto& [TextureType, Texture] : renderObject.Textures) {
					UINT* pTexture;
					auto& textures = objectResourceDescriptorHeapIndices.Textures;
					switch (TextureType) {
						case TextureType::BaseColorMap: pTexture = &textures.BaseColorMap; break;
						case TextureType::EmissiveColorMap: pTexture = &textures.EmissiveColorMap; break;
						case TextureType::MetallicMap: pTexture = &textures.MetallicMap; break;
						case TextureType::RoughnessMap: pTexture = &textures.RoughnessMap; break;
						case TextureType::AmbientOcclusionMap: pTexture = &textures.AmbientOcclusionMap; break;
						case TextureType::TransmissionMap: pTexture = &textures.TransmissionMap; break;
						case TextureType::OpacityMap: pTexture = &textures.OpacityMap; break;
						case TextureType::NormalMap: pTexture = &textures.NormalMap; break;
						default: pTexture = nullptr; break;
					}
					if (pTexture != nullptr) *pTexture = Texture.DescriptorHeapIndices.SRV;
				}

				objectIndex++;
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
		m_inputDevices.Mouse->ResetScrollWheelValue();

		const auto isUIVisible = m_UIStates.IsVisible;

		{
			if (gamepadStateTracker.menu == GamepadButtonState::PRESSED) m_UIStates.IsVisible = !m_UIStates.IsVisible;
			if (keyboardStateTracker.IsKeyPressed(Key::Escape)) m_UIStates.IsVisible = !m_UIStates.IsVisible;
		}

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
			else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) m_UIStates.HasFocus = false;
			if (m_UIStates.HasFocus) {
				IO.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;

				m_inputDevices.Mouse->SetMode(Mouse::MODE_ABSOLUTE);
			}
			else IO.ConfigFlags |= ImGuiConfigFlags_NoMouse;
		}

		if (!m_UIStates.IsVisible || !m_UIStates.HasFocus) {
			m_inputDevices.Mouse->SetMode(Mouse::MODE_RELATIVE);

			UpdateCamera();
		}
	}

	void ResetCamera() {
		m_cameraController.SetPosition(m_scene->Camera.Position);
		m_cameraController.SetRotation(m_scene->Camera.Rotation);
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
			if (movementSpeedIncrement) {
				speedSettings.Movement = clamp(speedSettings.Movement + elapsedSeconds * static_cast<float>(movementSpeedIncrement) * 12, 0.0f, CameraMaxMovementSpeed);

				ignore = g_controlsSettings.Save();
			}

			const auto movementSpeed = elapsedSeconds * speedSettings.Movement;
			displacement.x += gamepadState.thumbSticks.leftX * movementSpeed;
			displacement.z += gamepadState.thumbSticks.leftY * movementSpeed;

			const auto rotationSpeed = elapsedSeconds * XM_2PI * speedSettings.Rotation;
			yaw += gamepadState.thumbSticks.rightX * rotationSpeed;
			pitch += gamepadState.thumbSticks.rightY * rotationSpeed;
		}

		if (mouseState.positionMode == Mouse::MODE_RELATIVE) {
			if (m_inputDeviceStateTrackers.Keyboard.IsKeyPressed(Key::Home)) ResetCamera();

			if (mouseState.scrollWheelValue) {
				speedSettings.Movement = clamp(speedSettings.Movement + static_cast<float>(mouseState.scrollWheelValue) * 0.008f, 0.0f, CameraMaxMovementSpeed);

				ignore = g_controlsSettings.Save();
			}

			const auto movementSpeed = elapsedSeconds * speedSettings.Movement;
			if (keyboardState.A) displacement.x -= movementSpeed;
			if (keyboardState.D) displacement.x += movementSpeed;
			if (keyboardState.W) displacement.z += movementSpeed;
			if (keyboardState.S) displacement.z -= movementSpeed;

			const auto rotationSpeed = XM_2PI * 0.0004f * speedSettings.Rotation;
			yaw += static_cast<float>(mouseState.x) * rotationSpeed;
			pitch += static_cast<float>(-mouseState.y) * rotationSpeed;
		}

		if (const auto angle = asin(m_cameraController.GetNormalizedForwardDirection().y) + pitch;
			angle > XM_PIDIV2) pitch = max(0.0f, -angle + XM_PIDIV2 - 0.1f);
		else if (angle < -XM_PIDIV2) pitch = min(0.0f, -angle - XM_PIDIV2 + 0.1f);

		if (displacement == Vector3() && yaw == 0 && pitch == 0) return;

		m_cameraController.Move(m_cameraController.GetNormalizedRightDirection() * displacement.x + m_cameraController.GetNormalizedUpDirection() * displacement.y + m_cameraController.GetNormalizedForwardDirection() * displacement.z);
		m_cameraController.Rotate(yaw, pitch);
	}

	void UpdateGraphicsSettings() {
		const auto& raytracingSettings = g_graphicsSettings.Raytracing;
		m_GPUBuffers.GraphicsSettings->GetData() = {
			.FrameIndex = m_stepTimer.GetFrameCount() - 1,
			.MaxTraceRecursionDepth = raytracingSettings.MaxTraceRecursionDepth,
			.SamplesPerPixel = raytracingSettings.SamplesPerPixel,
			.IsRussianRouletteEnabled = raytracingSettings.IsRussianRouletteEnabled,
			.NRDHitDistanceParameters = reinterpret_cast<const XMFLOAT4&>(m_NRDReblurSettings.hitDistanceParameters)
		};
	}

	void UpdateScene() {
		if (!m_scene->IsWorldStatic()) {
			for (UINT instanceIndex = 0; const auto & renderObject : m_scene->RenderObjects) m_GPUBuffers.InstanceData->GetData(instanceIndex++).PreviousObjectToWorld = Transform(*renderObject.Shape);
		}

		m_scene->Tick(static_cast<PxReal>(min(1.0 / 60, m_stepTimer.GetElapsedSeconds())), m_inputDeviceStateTrackers.Gamepad, m_inputDeviceStateTrackers.Keyboard, m_inputDeviceStateTrackers.Mouse);

		m_GPUBuffers.SceneData->GetData() = {
			.IsStatic = m_scene->IsWorldStatic(),
			.EnvironmentLightColor = m_scene->EnvironmentLightColor,
			.EnvironmentLightCubeMapTransform = m_scene->EnvironmentLightCubeMap.Transform(),
			.EnvironmentColor = m_scene->EnvironmentColor,
			.EnvironmentCubeMapTransform = m_scene->EnvironmentCubeMap.Transform()
		};
	}

	void DispatchRays() {
		const auto commandList = m_deviceResources->GetCommandList();
		commandList->SetComputeRootSignature(m_rootSignature.Get());
		commandList->SetComputeRootShaderResourceView(0, m_topLevelAccelerationStructure->GetBuffer()->GetGPUVirtualAddress());
		commandList->SetComputeRoot32BitConstants(1, 2, &m_renderSize, 0);
		commandList->SetComputeRootConstantBufferView(2, m_GPUBuffers.GlobalResourceDescriptorHeapIndices->GetResource()->GetGPUVirtualAddress());
		commandList->SetPipelineState(m_pipelineState.Get());
		commandList->Dispatch((m_renderSize.x + 16) / 16, (m_renderSize.y + 16) / 16, 1);
	}

	auto CreateResourceTagInfo(BufferType type, const RenderTexture& renderTexture, bool isRenderSize = true, ResourceLifecycle lifecycle = ResourceLifecycle::eValidUntilEvaluate) const {
		ResourceTagInfo resourceTagInfo{
			.Type = type,
			.Resource = Resource(sl::ResourceType::eTex2d, renderTexture.GetResource(), renderTexture.GetCurrentState()),
			.Lifecycle = lifecycle
		};
		if (isRenderSize) resourceTagInfo.Extent = { .width = m_renderSize.x, .height = m_renderSize.y };
		return resourceTagInfo;
	}

	auto CreateResourceTagInfo(BufferType type, LPCSTR textureName, bool isRenderSize = true, ResourceLifecycle lifecycle = ResourceLifecycle::eValidUntilEvaluate) const {
		const auto& texture = *m_renderTextures.at(textureName);
		ResourceTagInfo resourceTagInfo{
			.Type = type,
			.Resource = Resource(sl::ResourceType::eTex2d, texture.GetResource(), texture.GetCurrentState()),
			.Lifecycle = lifecycle
		};
		if (isRenderSize) resourceTagInfo.Extent = { .width = m_renderSize.x, .height = m_renderSize.y };
		return resourceTagInfo;
	}

	bool IsDLSSEnabled() const { return g_graphicsSettings.PostProcessing.DLSS.IsEnabled && m_streamline->IsFeatureAvailable(kFeatureDLSS); }
	bool IsDLSSSuperResolutionEnabled() const { return IsDLSSEnabled() && g_graphicsSettings.PostProcessing.DLSS.SuperResolutionMode != DLSSSuperResolutionMode::Off; }

	bool IsNISEnabled() const { return g_graphicsSettings.PostProcessing.NIS.IsEnabled && m_streamline->IsFeatureAvailable(kFeatureNIS); }

	void SetDLSSOptimalSettings() {
		switch (auto& mode = m_DLSSOptions.mode; g_graphicsSettings.PostProcessing.DLSS.SuperResolutionMode) {
			case DLSSSuperResolutionMode::Auto:
			{
				const auto outputSize = m_deviceResources->GetOutputSize();
				if (const auto minValue = min(outputSize.cx, outputSize.cy);
					minValue <= 720) mode = DLSSMode::eDLAA;
				else if (minValue <= 1080) mode = DLSSMode::eMaxQuality;
				else if (minValue <= 1440) mode = DLSSMode::eBalanced;
				else if (minValue <= 2160) mode = DLSSMode::eMaxPerformance;
				else mode = DLSSMode::eUltraPerformance;
			}
			break;

			case DLSSSuperResolutionMode::DLAA: mode = DLSSMode::eDLAA; break;
			case DLSSSuperResolutionMode::Quality: mode = DLSSMode::eMaxQuality; break;
			case DLSSSuperResolutionMode::Balanced: mode = DLSSMode::eBalanced; break;
			case DLSSSuperResolutionMode::Performance: mode = DLSSMode::eMaxPerformance; break;
			case DLSSSuperResolutionMode::UltraPerformance: mode = DLSSMode::eUltraPerformance; break;
			default: mode = DLSSMode::eOff; break;
		}
		slDLSSGetOptimalSettings(m_DLSSOptions, m_DLSSOptimalSettings);
		m_DLSSOptions.sharpness = m_DLSSOptimalSettings.optimalSharpness;
		ignore = m_streamline->SetConstants(m_DLSSOptions);
	}

	void PostProcessGraphics() {
		const auto commandList = m_deviceResources->GetCommandList();

		const auto& postProcessingSettings = g_graphicsSettings.PostProcessing;

		PrepareStreamline();

		const auto isNRDEnabled = postProcessingSettings.NRD.IsEnabled && postProcessingSettings.NRD.SplitScreen != 1 && m_NRD->IsAvailable();

		if (isNRDEnabled) {
			ProcessNRD();

			m_NRDCommonSettings.accumulationMode = AccumulationMode::CONTINUE;
		}
		else m_NRDCommonSettings.accumulationMode = AccumulationMode::RESTART;

		auto input = m_renderTextures.at(RenderTextureNames::Output).get(), output = m_renderTextures.at(RenderTextureNames::FinalOutput).get();

		if (IsDLSSSuperResolutionEnabled()) {
			ProcessDLSS();

			m_slConstants.reset = Boolean::eFalse;

			swap(input, output);
		}
		else {
			m_slConstants.reset = Boolean::eTrue;

			if (auto& reset = m_temporalAntiAliasing->GetData().Reset; postProcessingSettings.IsTemporalAntiAliasingEnabled) {
				ProcessTemporalAntiAliasing();

				reset = false;

				swap(input, output);
			}
			else reset = true;
		}

		if (IsNISEnabled()) {
			ProcessNIS(*input, *output);

			swap(input, output);
		}

		if (postProcessingSettings.Bloom.IsEnabled) {
			ProcessBloom(*input, *output);

			swap(input, output);
		}

		ProcessToneMapping(*input);

		if (isNRDEnabled && postProcessingSettings.NRD.IsValidationLayerEnabled) {
			const auto& validation = *m_renderTextures.at(RenderTextureNames::Validation);

			const ScopedBarrier scopedBarrier(commandList, { CD3DX12_RESOURCE_BARRIER::Transition(validation.GetResource(), validation.GetCurrentState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE) });

			m_alphaBlending->SetViewport(m_deviceResources->GetScreenViewport());
			m_alphaBlending->Begin(commandList);
			m_alphaBlending->Draw(m_resourceDescriptorHeap->GetGpuHandle(validation.GetSrvDescriptorHeapIndex()), GetTextureSize(validation.GetResource()), XMFLOAT2());
			m_alphaBlending->End();
		}
	}

	void PrepareStreamline() {
		ignore = m_streamline->NewFrame();

		const auto& camera = m_GPUBuffers.Camera->GetData();
		m_slConstants.jitterOffset = { -camera.PixelJitter.x, -camera.PixelJitter.y };
		reinterpret_cast<XMFLOAT3&>(m_slConstants.cameraPos) = m_cameraController.GetPosition();
		reinterpret_cast<XMFLOAT3&>(m_slConstants.cameraUp) = m_cameraController.GetUpDirection();
		reinterpret_cast<XMFLOAT3&>(m_slConstants.cameraRight) = m_cameraController.GetRightDirection();
		reinterpret_cast<XMFLOAT3&>(m_slConstants.cameraFwd) = m_cameraController.GetForwardDirection();
		m_slConstants.cameraNear = m_cameraController.GetNearZ();
		m_slConstants.cameraFar = m_cameraController.GetFarZ();
		m_slConstants.cameraFOV = m_cameraController.GetVerticalFieldOfView();
		m_slConstants.cameraAspectRatio = m_cameraController.GetAspectRatio();
		reinterpret_cast<XMFLOAT4X4&>(m_slConstants.cameraViewToClip) = m_cameraController.GetViewToProjection();
		recalculateCameraMatrices(m_slConstants, reinterpret_cast<const float4x4&>(camera.PreviousViewToWorld), reinterpret_cast<const float4x4&>(camera.PreviousViewToProjection));

		m_slConstants.mvecScale = { 1 / static_cast<float>(m_renderSize.x), 1 / static_cast<float>(m_renderSize.y) };

		ignore = m_streamline->SetConstants(m_slConstants);
	}

	void ProcessNRD() {
		const auto commandList = m_deviceResources->GetCommandList();

		const auto
			& depth = *m_renderTextures.at(RenderTextureNames::Depth),
			& baseColorMetalness = *m_renderTextures.at(RenderTextureNames::BaseColorMetalness),
			& normalRoughness = *m_renderTextures.at(RenderTextureNames::NormalRoughness),
			& denoisedDiffuse = *m_renderTextures.at(RenderTextureNames::DenoisedDiffuse),
			& denoisedSpecular = *m_renderTextures.at(RenderTextureNames::DenoisedSpecular);

		{
			const auto SetResource = [&](nrd::ResourceType resourceType, const RenderTexture& texture) { m_NRD->SetResource(resourceType, texture.GetResource(), texture.GetCurrentState()); };
			SetResource(nrd::ResourceType::IN_VIEWZ, depth);
			SetResource(nrd::ResourceType::IN_MV, *m_renderTextures.at(RenderTextureNames::MotionVectors3D));
			SetResource(nrd::ResourceType::IN_BASECOLOR_METALNESS, baseColorMetalness);
			SetResource(nrd::ResourceType::IN_NORMAL_ROUGHNESS, normalRoughness);
			SetResource(nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST, *m_renderTextures.at(RenderTextureNames::NoisyDiffuse));
			SetResource(nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST, *m_renderTextures.at(RenderTextureNames::NoisySpecular));
			SetResource(nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST, denoisedDiffuse);
			SetResource(nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST, denoisedSpecular);
			SetResource(nrd::ResourceType::OUT_VALIDATION, *m_renderTextures.at(RenderTextureNames::Validation));

			const auto& camera = m_GPUBuffers.Camera->GetData();
			reinterpret_cast<XMFLOAT4X4&>(m_NRDCommonSettings.worldToViewMatrixPrev) = camera.PreviousWorldToView;
			reinterpret_cast<XMFLOAT4X4&>(m_NRDCommonSettings.viewToClipMatrixPrev) = camera.PreviousViewToProjection;
			reinterpret_cast<XMFLOAT4X4&>(m_NRDCommonSettings.worldToViewMatrix) = m_cameraController.GetWorldToView();
			reinterpret_cast<XMFLOAT4X4&>(m_NRDCommonSettings.viewToClipMatrix) = m_cameraController.GetViewToProjection();
			reinterpret_cast<XMFLOAT2&>(m_NRDCommonSettings.cameraJitter) = camera.PixelJitter;

			const auto outputSize = m_deviceResources->GetOutputSize();
			reinterpret_cast<XMFLOAT2&>(m_NRDCommonSettings.resolutionScale) = { static_cast<float>(m_renderSize.x) / static_cast<float>(outputSize.cx), static_cast<float>(m_renderSize.y) / static_cast<float>(outputSize.cy) };
			reinterpret_cast<XMFLOAT3&>(m_NRDCommonSettings.motionVectorScale) = { 1 / static_cast<float>(m_renderSize.x), 1 / static_cast<float>(m_renderSize.y), 1 };

			const auto& NRDSettings = g_graphicsSettings.PostProcessing.NRD;
			m_NRDCommonSettings.splitScreen = NRDSettings.SplitScreen;
			m_NRDCommonSettings.enableValidation = NRDSettings.IsValidationLayerEnabled;

			m_NRD->SetMethodSettings(Method::REBLUR_DIFFUSE_SPECULAR, &m_NRDReblurSettings);

			m_NRD->Denoise(m_stepTimer.GetFrameCount() - 1, m_NRDCommonSettings);
		}

		const auto descriptorHeap = m_resourceDescriptorHeap->Heap();
		commandList->SetDescriptorHeaps(1, &descriptorHeap);

		{
			const ScopedBarrier scopedBarrier(
				commandList,
				{
					CD3DX12_RESOURCE_BARRIER::Transition(depth.GetResource(), depth.GetCurrentState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(baseColorMetalness.GetResource(), baseColorMetalness.GetCurrentState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(normalRoughness.GetResource(), normalRoughness.GetCurrentState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(denoisedDiffuse.GetResource(), denoisedDiffuse.GetCurrentState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(denoisedSpecular.GetResource(), denoisedSpecular.GetCurrentState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE)
				}
			);

			m_denoisedComposition->Descriptors = {
				.InDepth = m_resourceDescriptorHeap->GetGpuHandle(depth.GetSrvDescriptorHeapIndex()),
				.InBaseColorMetalness = m_resourceDescriptorHeap->GetGpuHandle(baseColorMetalness.GetSrvDescriptorHeapIndex()),
				.InNormalRoughness = m_resourceDescriptorHeap->GetGpuHandle(normalRoughness.GetSrvDescriptorHeapIndex()),
				.InDenoisedDiffuse = m_resourceDescriptorHeap->GetGpuHandle(denoisedDiffuse.GetSrvDescriptorHeapIndex()),
				.InDenoisedSpecular = m_resourceDescriptorHeap->GetGpuHandle(denoisedSpecular.GetSrvDescriptorHeapIndex()),
				.Output = m_resourceDescriptorHeap->GetGpuHandle(m_renderTextures.at(RenderTextureNames::Output)->GetUavDescriptorHeapIndex())
			};

			m_denoisedComposition->RenderSize = m_renderSize;

			m_denoisedComposition->GetData() = {
				.CameraRightDirection = m_cameraController.GetRightDirection(),
				.CameraUpDirection = m_cameraController.GetUpDirection(),
				.CameraForwardDirection = m_cameraController.GetForwardDirection(),
				.CameraPixelJitter = m_GPUBuffers.Camera->GetData().PixelJitter
			};

			m_denoisedComposition->Process(commandList);
		}
	}

	void ProcessTemporalAntiAliasing() {
		const auto commandList = m_deviceResources->GetCommandList();

		const auto& historyOutput = *m_renderTextures.at(RenderTextureNames::HistoryOutput), & finalOutput = *m_renderTextures.at(RenderTextureNames::FinalOutput);

		{
			const auto& output = *m_renderTextures.at(RenderTextureNames::Output), & motionVectors3D = *m_renderTextures.at(RenderTextureNames::MotionVectors3D);

			const ScopedBarrier scopedBarrier(
				commandList,
				{
					CD3DX12_RESOURCE_BARRIER::Transition(historyOutput.GetResource(), historyOutput.GetCurrentState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(output.GetResource(), output.GetCurrentState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(motionVectors3D.GetResource(), motionVectors3D.GetCurrentState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE)
				}
			);

			m_temporalAntiAliasing->Descriptors = {
				.InHistoryOutput = m_resourceDescriptorHeap->GetGpuHandle(historyOutput.GetSrvDescriptorHeapIndex()),
				.InCurrentOutput = m_resourceDescriptorHeap->GetGpuHandle(output.GetSrvDescriptorHeapIndex()),
				.InMotionVectors3D = m_resourceDescriptorHeap->GetGpuHandle(motionVectors3D.GetSrvDescriptorHeapIndex()),
				.OutFinalOutput = m_resourceDescriptorHeap->GetGpuHandle(finalOutput.GetUavDescriptorHeapIndex())
			};

			m_temporalAntiAliasing->RenderSize = m_renderSize;

			m_temporalAntiAliasing->GetData() = {
				.CameraPosition = m_cameraController.GetPosition(),
				.CameraRightDirection = m_cameraController.GetRightDirection(),
				.CameraUpDirection = m_cameraController.GetUpDirection(),
				.CameraForwardDirection = m_cameraController.GetForwardDirection(),
				.CameraPreviousWorldToProjection = m_GPUBuffers.Camera->GetData().PreviousWorldToProjection
			};

			m_temporalAntiAliasing->Process(commandList);
		}

		{
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

	void ProcessDLSS() {
		const auto commandList = m_deviceResources->GetCommandList();

		const auto
			& depth = *m_renderTextures.at(RenderTextureNames::Depth),
			& motionVectors2D = *m_renderTextures.at(RenderTextureNames::MotionVectors2D),
			& motionVectors3D = *m_renderTextures.at(RenderTextureNames::MotionVectors3D);

		{
			const ScopedBarrier scopedBarrier(commandList, { CD3DX12_RESOURCE_BARRIER::Transition(motionVectors3D.GetResource(), motionVectors3D.GetCurrentState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE) });

			m_preDLSS->Descriptors = {
				.InMotionVectors3D = m_resourceDescriptorHeap->GetGpuHandle(motionVectors3D.GetSrvDescriptorHeapIndex()),
				.InOutDepth = m_resourceDescriptorHeap->GetGpuHandle(depth.GetUavDescriptorHeapIndex()),
				.OutMotionVectors2D = m_resourceDescriptorHeap->GetGpuHandle(motionVectors2D.GetUavDescriptorHeapIndex())
			};

			m_preDLSS->RenderSize = m_renderSize;

			m_preDLSS->GetData() = {
				.CameraPosition = m_cameraController.GetPosition(),
				.CameraRightDirection = m_cameraController.GetRightDirection(),
				.CameraUpDirection = m_cameraController.GetUpDirection(),
				.CameraForwardDirection = m_cameraController.GetForwardDirection(),
				.CameraWorldToProjection = m_cameraController.GetWorldToProjection()
			};

			m_preDLSS->Process(commandList);
		}

		{
			ResourceTagInfo resourceTagInfos[]{
				CreateResourceTagInfo(kBufferTypeDepth, depth),
				CreateResourceTagInfo(kBufferTypeMotionVectors, motionVectors2D),
				CreateResourceTagInfo(kBufferTypeScalingInputColor, RenderTextureNames::Output),
				CreateResourceTagInfo(kBufferTypeScalingOutputColor, RenderTextureNames::FinalOutput, false)
			};
			ignore = m_streamline->EvaluateFeature(kFeatureDLSS, CreateResourceTags(resourceTagInfos));
		}

		const auto descriptorHeap = m_resourceDescriptorHeap->Heap();
		commandList->SetDescriptorHeaps(1, &descriptorHeap);
	}

	void ProcessNIS(RenderTexture& input, RenderTexture& output) {
		const auto commandList = m_deviceResources->GetCommandList();

		NISOptions NISOptions;
		NISOptions.mode = NISMode::eSharpen;
		NISOptions.sharpness = g_graphicsSettings.PostProcessing.NIS.Sharpness;
		ignore = m_streamline->SetConstants(NISOptions);

		ResourceTagInfo resourceTagInfos[]{
			CreateResourceTagInfo(kBufferTypeScalingInputColor, input, false),
			CreateResourceTagInfo(kBufferTypeScalingOutputColor, output, false)
		};
		ignore = m_streamline->EvaluateFeature(kFeatureNIS, CreateResourceTags(resourceTagInfos));

		const auto descriptorHeap = m_resourceDescriptorHeap->Heap();
		commandList->SetDescriptorHeaps(1, &descriptorHeap);
	}

	void ProcessBloom(RenderTexture& input, RenderTexture& output) {
		const auto commandList = m_deviceResources->GetCommandList();

		const auto& bloomSettings = g_graphicsSettings.PostProcessing.Bloom;

		const auto& [Extraction, Blur, Combination] = m_bloom;

		auto& blur1 = *m_renderTextures.at(RenderTextureNames::Blur1), & blur2 = *m_renderTextures.at(RenderTextureNames::Blur2);

		const auto inputState = input.GetCurrentState(), blur1State = blur1.GetCurrentState(), blur2State = blur2.GetCurrentState();

		const auto
			inputSRV = m_resourceDescriptorHeap->GetGpuHandle(input.GetSrvDescriptorHeapIndex()),
			blur1SRV = m_resourceDescriptorHeap->GetGpuHandle(blur1.GetSrvDescriptorHeapIndex()),
			blur2SRV = m_resourceDescriptorHeap->GetGpuHandle(blur2.GetSrvDescriptorHeapIndex());

		const auto
			blur1RTV = m_renderDescriptorHeap->GetCpuHandle(blur1.GetRtvDescriptorHeapIndex()),
			blur2RTV = m_renderDescriptorHeap->GetCpuHandle(blur2.GetRtvDescriptorHeapIndex());

		input.TransitionTo(commandList, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

		Extraction->SetSourceTexture(inputSRV, input.GetResource());
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

		Combination->SetSourceTexture(inputSRV);
		Combination->SetSourceTexture2(blur1SRV);
		Combination->SetBloomCombineParameters(1.25f, 1, 1, 1);

		auto renderTargetView = m_renderDescriptorHeap->GetCpuHandle(output.GetRtvDescriptorHeapIndex());

		commandList->OMSetRenderTargets(1, &renderTargetView, FALSE, nullptr);

		commandList->RSSetViewports(1, &viewPort);

		Combination->Process(commandList);

		input.TransitionTo(commandList, inputState);
		blur1.TransitionTo(commandList, blur1State);
		blur2.TransitionTo(commandList, blur2State);

		renderTargetView = m_deviceResources->GetRenderTargetView();
		commandList->OMSetRenderTargets(1, &renderTargetView, FALSE, nullptr);
	}

	void ProcessToneMapping(RenderTexture& input) {
		const auto commandList = m_deviceResources->GetCommandList();

		const ScopedBarrier scopedBarrier(commandList, { CD3DX12_RESOURCE_BARRIER::Transition(input.GetResource(), input.GetCurrentState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE) });

		auto& toneMapping = *m_toneMapping[ToneMapPostProcess::None];
		toneMapping.SetHDRSourceTexture(m_resourceDescriptorHeap->GetGpuHandle(input.GetSrvDescriptorHeapIndex()));
		toneMapping.Process(commandList);
	}

	void RenderUI() {
		ImGui_ImplDX12_NewFrame();
		ImGui_ImplWin32_NewFrame();

		const auto outputSize = GetOutputSize();
		ImGui::GetIO().DisplaySize = { static_cast<float>(outputSize.cx), static_cast<float>(outputSize.cy) };

		ImGui::NewFrame();

		const auto openPopupModalName = RenderMenuBar();

		if (m_UIStates.IsSettingsWindowVisible) RenderSettingsWindow();

		RenderPopupModalWindow(openPopupModalName);

		if (m_futures.contains(FutureNames::Scene)) RenderLoadingSceneWindow();

		ImGui::Render();
		ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), m_deviceResources->GetCommandList());
	}

	string RenderMenuBar() {
		string openPopupModalName;

		if (ImGui::BeginMainMenuBar()) {
			if (ImGui::GetFrameCount() == 1) ImGui::SetKeyboardFocusHere();

			const auto PopupModal = [&](LPCSTR name) { if (ImGui::MenuItem(name)) openPopupModalName = name; };

			if (ImGui::BeginMenu("File")) {
				if (ImGui::MenuItem("Exit")) PostQuitMessage(ERROR_SUCCESS);

				ImGui::EndMenu();
			}

			if (ImGui::BeginMenu("View")) {
				m_UIStates.IsSettingsWindowVisible |= ImGui::MenuItem("Settings");

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

		return openPopupModalName;
	}

	void RenderSettingsWindow() {
		ImGui::SetNextWindowBgAlpha(g_UISettings.WindowOpacity);

		const auto& viewport = *ImGui::GetMainViewport();
		ImGui::SetNextWindowPos({ viewport.WorkPos.x, viewport.WorkPos.y });
		ImGui::SetNextWindowSize({});

		if (ImGui::Begin("Settings", &m_UIStates.IsSettingsWindowVisible, ImGuiWindowFlags_HorizontalScrollbar)) {
			if (ImGui::TreeNode("Graphics")) {
				auto isChanged = false;

				{
					auto isWindowSettingChanged = false;

					if (const auto mode = m_windowModeHelper->GetMode();
						ImGui::BeginCombo("Window Mode", ToString(mode))) {
						for (const auto windowMode : { WindowMode::Windowed, WindowMode::Borderless, WindowMode::Fullscreen }) {
							const auto isSelected = mode == windowMode;

							if (ImGui::Selectable(ToString(windowMode), isSelected)) {
								g_graphicsSettings.WindowMode = windowMode;

								m_windowModeHelper->SetMode(windowMode);

								isChanged = isWindowSettingChanged = true;
							}

							if (isSelected) ImGui::SetItemDefaultFocus();
						}

						ImGui::EndCombo();
					}

					{
						const auto ToString = [](SIZE value) { return format("{} × {}", value.cx, value.cy); };
						if (const auto resolution = m_windowModeHelper->GetResolution();
							ImGui::BeginCombo("Resolution", ToString(resolution).c_str())) {
							for (const auto& displayResolution : g_displayResolutions) {
								const auto isSelected = resolution == displayResolution;

								if (ImGui::Selectable(ToString(displayResolution).c_str(), isSelected)) {
									g_graphicsSettings.Resolution = displayResolution;

									m_windowModeHelper->SetResolution(displayResolution);

									isChanged = isWindowSettingChanged = true;
								}

								if (isSelected) ImGui::SetItemDefaultFocus();
							}

							ImGui::EndCombo();
						}
					}

					if (isWindowSettingChanged) m_futures["WindowSetting"] = async(launch::deferred, [&] { ThrowIfFailed(m_windowModeHelper->Apply()); });
				}

				{
					const ImGuiEx::ScopedEnablement scopedEnablement(m_deviceResources->GetDeviceOptions() & DeviceResources::c_AllowTearing);

					if (auto isEnabled = m_deviceResources->IsVSyncEnabled(); ImGui::Checkbox("V-Sync", &isEnabled) && m_deviceResources->EnableVSync(isEnabled)) {
						g_graphicsSettings.IsVSyncEnabled = isEnabled;

						isChanged = true;
					}
				}

				if (ImGui::TreeNodeEx("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
					auto& cameraSettings = g_graphicsSettings.Camera;

					isChanged |= ImGui::Checkbox("Jitter", &cameraSettings.IsJitterEnabled);

					if (ImGui::SliderFloat("Vertical Field of View", &cameraSettings.VerticalFieldOfView, CameraMinVerticalFieldOfView, CameraMaxVerticalFieldOfView, "%.1f°", ImGuiSliderFlags_AlwaysClamp)) {
						m_cameraController.SetLens(XMConvertToRadians(cameraSettings.VerticalFieldOfView), m_cameraController.GetAspectRatio());

						isChanged = true;
					}

					ImGui::TreePop();
				}

				if (ImGui::TreeNodeEx("Raytracing", ImGuiTreeNodeFlags_DefaultOpen)) {
					auto& raytracingSettings = g_graphicsSettings.Raytracing;

					isChanged |= ImGui::Checkbox("Russian Roulette", &raytracingSettings.IsRussianRouletteEnabled);

					isChanged |= ImGui::SliderInt("Max Trace Recursion Depth", reinterpret_cast<int*>(&raytracingSettings.MaxTraceRecursionDepth), 1, MaxTraceRecursionDepth, "%d", ImGuiSliderFlags_AlwaysClamp);

					isChanged |= ImGui::SliderInt("Samples Per Pixel", reinterpret_cast<int*>(&raytracingSettings.SamplesPerPixel), 1, MaxSamplesPerPixel, "%d", ImGuiSliderFlags_AlwaysClamp);

					ImGui::TreePop();
				}

				if (ImGui::TreeNodeEx("Post-Processing", ImGuiTreeNodeFlags_DefaultOpen)) {
					auto& postProcessingSetttings = g_graphicsSettings.PostProcessing;

					{
						const auto isAvailable = m_NRD->IsAvailable();

						const ImGuiEx::ScopedEnablement scopedEnablement(isAvailable);

						if (ImGui::TreeNodeEx("NVIDIA Real-Time Denoisers", ImGuiTreeNodeFlags_DefaultOpen)) {
							auto& NRDSettings = postProcessingSetttings.NRD;

							auto isEnabled = NRDSettings.IsEnabled && isAvailable;

							{
								ImGui::PushID("Enable NVIDIA Real-Time Denoisers");

								if (ImGui::Checkbox("Enable", &isEnabled)) {
									NRDSettings.IsEnabled = isEnabled;

									isChanged = true;
								}

								ImGui::PopID();
							}

							isChanged |= isEnabled && ImGui::Checkbox("Validation Layer", &NRDSettings.IsValidationLayerEnabled);

							isChanged |= isEnabled && ImGui::SliderFloat("Split Screen", &NRDSettings.SplitScreen, 0, 1, "%.2f", ImGuiSliderFlags_AlwaysClamp);

							ImGui::TreePop();
						}
					}

					if (!IsDLSSSuperResolutionEnabled()) isChanged |= ImGui::Checkbox("Temporal Anti-Aliasing", &postProcessingSetttings.IsTemporalAntiAliasingEnabled);

					{
						const ImGuiEx::ScopedEnablement scopedEnablement(m_streamline->IsFeatureAvailable(kFeatureDLSS));

						if (ImGui::TreeNodeEx("NVIDIA DLSS", ImGuiTreeNodeFlags_DefaultOpen)) {
							auto& DLSSSettings = postProcessingSetttings.DLSS;

							auto isEnabled = IsDLSSEnabled();

							{
								ImGui::PushID("Enable NVIDIA DLSS");

								if (ImGui::Checkbox("Enable", &isEnabled)) {
									DLSSSettings.IsEnabled = isEnabled;

									isChanged = true;
								}

								ImGui::PopID();
							}

							if (isEnabled) {
								if (ImGui::BeginCombo("Super Resolution", ToString(DLSSSettings.SuperResolutionMode))) {
									auto isDLSSSuperResolutionModeSettingChanged = false;
									for (const auto DLSSSuperResolutionMode : { DLSSSuperResolutionMode::Off, DLSSSuperResolutionMode::Auto, DLSSSuperResolutionMode::DLAA, DLSSSuperResolutionMode::Quality, DLSSSuperResolutionMode::Balanced, DLSSSuperResolutionMode::Performance, DLSSSuperResolutionMode::UltraPerformance }) {
										const auto isSelected = DLSSSettings.SuperResolutionMode == DLSSSuperResolutionMode;

										if (ImGui::Selectable(ToString(DLSSSuperResolutionMode), isSelected)) {
											DLSSSettings.SuperResolutionMode = DLSSSuperResolutionMode;

											isDLSSSuperResolutionModeSettingChanged = true;

											isChanged = true;
										}

										if (isSelected) ImGui::SetItemDefaultFocus();
									}

									ImGui::EndCombo();

									if (isDLSSSuperResolutionModeSettingChanged) m_futures["DLSSSuperResolutionModeSetting"] = async(launch::deferred, [&] { SetDLSSOptimalSettings(); });
								}
							}

							ImGui::TreePop();
						}
					}

					{
						const ImGuiEx::ScopedEnablement scopedEnablement(m_streamline->IsFeatureAvailable(kFeatureNIS));

						if (ImGui::TreeNodeEx("NVIDIA Image Scaling", ImGuiTreeNodeFlags_DefaultOpen)) {
							auto& NISSettings = postProcessingSetttings.NIS;

							auto isEnabled = IsNISEnabled();

							{
								ImGui::PushID("Enable NVIDIA Image Scaling");

								if (ImGui::Checkbox("Enable", &isEnabled)) {
									NISSettings.IsEnabled = isEnabled;

									isChanged = true;
								}

								ImGui::PopID();
							}

							isChanged |= isEnabled && ImGui::SliderFloat("Sharpness", &NISSettings.Sharpness, 0, 1, "%.2f", ImGuiSliderFlags_AlwaysClamp);

							ImGui::TreePop();
						}
					}

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

				if (isChanged) ignore = g_graphicsSettings.Save();
			}

			if (ImGui::TreeNode("UI")) {
				auto isChanged = false;

				isChanged |= ImGui::Checkbox("Show on Startup", &g_UISettings.ShowOnStartup);

				isChanged |= ImGui::SliderFloat("Window Opacity", &g_UISettings.WindowOpacity, 0, 1, "%.2f", ImGuiSliderFlags_AlwaysClamp);

				ImGui::TreePop();

				if (isChanged) ignore = g_UISettings.Save();
			}

			if (ImGui::TreeNode("Controls")) {
				auto isChanged = false;

				if (ImGui::TreeNodeEx("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
					auto& cameraSettings = g_controlsSettings.Camera;

					if (ImGui::TreeNodeEx("Speed", ImGuiTreeNodeFlags_DefaultOpen)) {
						auto& speedSettings = cameraSettings.Speed;

						isChanged |= ImGui::SliderFloat("Movement", &speedSettings.Movement, 0.0f, CameraMaxMovementSpeed, "%.1f", ImGuiSliderFlags_AlwaysClamp);

						isChanged |= ImGui::SliderFloat("Rotation", &speedSettings.Rotation, 0.0f, CameraMaxRotationSpeed, "%.2f", ImGuiSliderFlags_AlwaysClamp);

						ImGui::TreePop();
					}

					ImGui::TreePop();
				}

				ImGui::TreePop();

				if (isChanged) ignore = g_controlsSettings.Save();
			}
		}

		ImGui::End();
	}

	void RenderPopupModalWindow(const string& openPopupModalName) {
		const auto PopupModal = [&](LPCSTR name, const auto& lambda) {
			if (name == openPopupModalName) ImGui::OpenPopup(name);

			ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetWorkCenter(), ImGuiCond_Always, { 0.5f, 0.5f });
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
				{ "Ctrl + Tab (hold)", "Show window switcher if UI visible" },
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
