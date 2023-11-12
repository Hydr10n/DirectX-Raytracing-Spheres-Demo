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

#include "rtxmu/D3D12AccelStructManager.h"

#include "sl_helpers.h"

#include "NRD.h"

#include "PhysX.h"

#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"
#include "ImGuiEx.h"

#include "MyAppData.h"

#include <shellapi.h>

#include "Shaders/Raytracing.dxil.h"

module App;

import Camera;
import DeviceResources;
import DirectX.BufferHelpers;
import DirectX.CommandList;
import DirectX.DescriptorHeap;
import DirectX.PostProcess.ChromaticAberration;
import DirectX.PostProcess.DenoisedComposition;
import DirectX.PostProcess.TemporalAntiAliasing;
import DirectX.RaytracingHelpers;
import HaltonSamplePattern;
import Model;
import MyScene;
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
using namespace rtxmu;
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
		m_deviceResources->WaitForGpu();

		{
			if (ImGui::GetIO().BackendRendererUserData != nullptr) ImGui_ImplDX12_Shutdown();

			ImGui_ImplWin32_Shutdown();

			ImGui::DestroyContext();
		}
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
		m_bottomLevelAccelerationStructureIDs = {};
		m_accelerationStructureManager.reset();

		m_alphaBlending.reset();

		for (auto& toneMapping : m_toneMapping) toneMapping.reset();

		m_bloom = {};

		m_chromaticAberration.reset();

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

	unique_ptr<DeviceResources> m_deviceResources = make_unique<DeviceResources>(DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_UNKNOWN, 2, D3D_FEATURE_LEVEL_12_1, D3D12_RAYTRACING_TIER_1_1, DeviceResources::c_AllowTearing);

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

	mutex m_exceptionMutex;
	exception_ptr m_exception;

	struct ResourceDescriptorHeapIndex {
		enum {
			InGraphicsSettings,
			InCamera,
			InSceneData,
			InInstanceData,
			InObjectResourceDescriptorHeapIndices, InObjectData,
			InHistoryColor, OutHistoryColor,
			InColor, OutColor,
			InFinalColor, OutFinalColor,
			InLinearDepth, OutLinearDepth,
			InNormalizedDepth, OutNormalizedDepth,
			InMotionVectors, OutMotionVectors,
			InBaseColorMetalness, OutBaseColorMetalness,
			InEmissiveColor, OutEmissiveColor,
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
			Color,
			FinalColor,
			Blur1, Blur2,
			Count
		};
	};
	unique_ptr<DescriptorHeap> m_renderDescriptorHeap;

	ComPtr<ID3D12RootSignature> m_rootSignature;

	ComPtr<ID3D12PipelineState> m_pipelineState;

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

	CommonSettings m_NRDCommonSettings{ .isBaseColorMetalnessAvailable = true };
	ReblurSettings m_NRDReblurSettings{ .hitDistanceReconstructionMode = HitDistanceReconstructionMode::AREA_3X3, .enableAntiFirefly = true };
	RelaxDiffuseSpecularSettings m_NRDRelaxSettings{ .hitDistanceReconstructionMode = HitDistanceReconstructionMode::AREA_3X3, .enableAntiFirefly = true };
	unique_ptr<NRD> m_NRD;
	unique_ptr<DenoisedComposition> m_denoisedComposition;

	unique_ptr<TemporalAntiAliasing> m_temporalAntiAliasing;

	DLSSOptions m_DLSSOptions;

	unique_ptr<ChromaticAberration> m_chromaticAberration;

	struct {
		unique_ptr<BasicPostProcess> Extraction, Blur;
		unique_ptr<DualPostProcess> Combination;
	} m_bloom;

	unique_ptr<ToneMapPostProcess> m_toneMapping[ToneMapPostProcess::Operator_Max];

	unique_ptr<SpriteBatch> m_alphaBlending;

	unique_ptr<DxAccelStructManager> m_accelerationStructureManager;
	unordered_map<shared_ptr<Mesh>, uint64_t> m_bottomLevelAccelerationStructureIDs;
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
		MAKE_NAME(HistoryColor);
		MAKE_NAME(Color);
		MAKE_NAME(FinalColor);
		MAKE_NAME(LinearDepth);
		MAKE_NAME(NormalizedDepth);
		MAKE_NAME(MotionVectors);
		MAKE_NAME(BaseColorMetalness);
		MAKE_NAME(EmissiveColor);
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

	CameraController m_cameraController;

	unique_ptr<Scene> m_scene;

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

		m_renderSize = XMUINT2(outputSize.cx, outputSize.cy);

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

			CreateTexture1(DXGI_FORMAT_R16G16B16A16_FLOAT, RenderTextureNames::HistoryColor, ResourceDescriptorHeapIndex::InHistoryColor, ResourceDescriptorHeapIndex::OutHistoryColor);
			CreateTexture1(DXGI_FORMAT_R16G16B16A16_FLOAT, RenderTextureNames::Color, ResourceDescriptorHeapIndex::InColor, ResourceDescriptorHeapIndex::OutColor, RenderDescriptorHeapIndex::Color);
			CreateTexture1(DXGI_FORMAT_R16G16B16A16_FLOAT, RenderTextureNames::FinalColor, ResourceDescriptorHeapIndex::InFinalColor, ResourceDescriptorHeapIndex::OutFinalColor, RenderDescriptorHeapIndex::FinalColor);
			CreateTexture1(DXGI_FORMAT_R32_FLOAT, RenderTextureNames::LinearDepth, ResourceDescriptorHeapIndex::InLinearDepth, ResourceDescriptorHeapIndex::OutLinearDepth, ~0u);
			CreateTexture1(DXGI_FORMAT_R32_FLOAT, RenderTextureNames::NormalizedDepth, ResourceDescriptorHeapIndex::InNormalizedDepth, ResourceDescriptorHeapIndex::OutNormalizedDepth, ~0u);
			CreateTexture1(DXGI_FORMAT_R16G16B16A16_FLOAT, RenderTextureNames::MotionVectors, ResourceDescriptorHeapIndex::InMotionVectors, ResourceDescriptorHeapIndex::OutMotionVectors);
			CreateTexture1(DXGI_FORMAT_R8G8B8A8_UNORM, RenderTextureNames::BaseColorMetalness, ResourceDescriptorHeapIndex::InBaseColorMetalness, ResourceDescriptorHeapIndex::OutBaseColorMetalness);
			CreateTexture1(DXGI_FORMAT_R11G11B10_FLOAT, RenderTextureNames::EmissiveColor, ResourceDescriptorHeapIndex::InEmissiveColor, ResourceDescriptorHeapIndex::OutEmissiveColor);

			if (m_NRD = make_unique<NRD>(
				device, m_deviceResources->GetCommandQueue(), m_deviceResources->GetCommandList(),
				m_deviceResources->GetBackBufferCount(),
				initializer_list<DenoiserDesc>{
					{ static_cast<Identifier>(NRDDenoiser::ReBLUR), Denoiser::REBLUR_DIFFUSE_SPECULAR, static_cast<uint16_t>(outputSize.cx), static_cast<UINT16>(outputSize.cy) },
					{ static_cast<Identifier>(NRDDenoiser::ReLAX), Denoiser::RELAX_DIFFUSE_SPECULAR, static_cast<uint16_t>(outputSize.cx), static_cast<UINT16>(outputSize.cy) }
			}
			);
				m_NRD->IsAvailable()) {
				CreateTexture1(NRD::ToDXGIFormat(GetLibraryDesc().normalEncoding), RenderTextureNames::NormalRoughness, ResourceDescriptorHeapIndex::InNormalRoughness, ResourceDescriptorHeapIndex::OutNormalRoughness);
				CreateTexture1(DXGI_FORMAT_R16G16B16A16_FLOAT, RenderTextureNames::NoisyDiffuse, ~0u, ResourceDescriptorHeapIndex::OutNoisyDiffuse);
				CreateTexture1(DXGI_FORMAT_R16G16B16A16_FLOAT, RenderTextureNames::NoisySpecular, ~0u, ResourceDescriptorHeapIndex::OutNoisySpecular);
				CreateTexture1(DXGI_FORMAT_R16G16B16A16_FLOAT, RenderTextureNames::DenoisedDiffuse, ResourceDescriptorHeapIndex::InDenoisedDiffuse, ResourceDescriptorHeapIndex::OutDenoisedDiffuse);
				CreateTexture1(DXGI_FORMAT_R16G16B16A16_FLOAT, RenderTextureNames::DenoisedSpecular, ResourceDescriptorHeapIndex::InDenoisedSpecular, ResourceDescriptorHeapIndex::OutDenoisedSpecular);
				CreateTexture1(DXGI_FORMAT_R8G8B8A8_UNORM, RenderTextureNames::Validation, ResourceDescriptorHeapIndex::InValidation, ResourceDescriptorHeapIndex::OutValidation);
			}
			else {
				CreateTexture1(DXGI_FORMAT_R16G16B16A16_SNORM, RenderTextureNames::NormalRoughness, ResourceDescriptorHeapIndex::InNormalRoughness, ResourceDescriptorHeapIndex::OutNormalRoughness);
			}

			if (m_streamline->IsFeatureAvailable(kFeatureDLSS)) {
				m_DLSSOptions.outputWidth = outputSize.cx;
				m_DLSSOptions.outputHeight = outputSize.cy;
				if (IsDLSSSuperResolutionEnabled()) SetDLSSOptimalSettings();
			}

			{
				const SIZE size{ outputSize.cx / 2, outputSize.cy / 2 };
				CreateTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, size, RenderTextureNames::Blur1, ResourceDescriptorHeapIndex::InBlur1, ~0u, RenderDescriptorHeapIndex::Blur1);
				CreateTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, size, RenderTextureNames::Blur2, ResourceDescriptorHeapIndex::InBlur2, ~0u, RenderDescriptorHeapIndex::Blur2);
			}
		}

		OnRenderSizeChanged();

		m_cameraController.SetLens(XMConvertToRadians(g_graphicsSettings.Camera.HorizontalFieldOfView), static_cast<float>(outputSize.cx) / static_cast<float>(outputSize.cy));

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
			camera.NearDepth = m_cameraController.GetNearDepth();
			camera.FarDepth = m_cameraController.GetFarDepth();
			camera.PixelJitter = g_graphicsSettings.Camera.IsJitterEnabled ? m_haltonSamplePattern.GetNext() : XMFLOAT2();
			camera.WorldToProjection = m_cameraController.GetWorldToProjection();
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

		const auto renderTargetView = m_deviceResources->GetRenderTargetView();
		commandList->OMSetRenderTargets(1, &renderTargetView, FALSE, nullptr);
		commandList->ClearRenderTargetView(renderTargetView, Colors::Black, 0, nullptr);

		const auto viewport = m_deviceResources->GetScreenViewport();
		const auto scissorRect = m_deviceResources->GetScissorRect();
		commandList->RSSetViewports(1, &viewport);
		commandList->RSSetScissorRects(1, &scissorRect);

		PIXEndEvent(commandList);

		PIXBeginEvent(commandList, PIX_COLOR_DEFAULT, L"Render");

		const auto descriptorHeap = m_resourceDescriptorHeap->Heap();
		commandList->SetDescriptorHeaps(1, &descriptorHeap);

		if (!m_futures.contains(FutureNames::Scene) && m_scene) {
			if (!m_scene->IsStatic()) CreateAccelerationStructures(true);
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

			CreateAccelerationStructures(false);

			{
				auto& globalResourceDescriptorHeapIndices = m_GPUBuffers.GlobalResourceDescriptorHeapIndices->GetData();
				globalResourceDescriptorHeapIndices.InEnvironmentLightTexture = m_scene->EnvironmentLightTexture.DescriptorHeapIndices.SRV;
				globalResourceDescriptorHeapIndices.InEnvironmentTexture = m_scene->EnvironmentTexture.DescriptorHeapIndices.SRV;
			}

			{
				const auto IsCubeMap = [](ID3D12Resource* pResource) {
					if (pResource != nullptr) {
						const auto desc = pResource->GetDesc();
						return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.DepthOrArraySize == 6;
					}
					return false;
				};
				auto& globalData = m_GPUBuffers.SceneData->GetData();
				globalData.IsEnvironmentLightTextureCubeMap = IsCubeMap(m_scene->EnvironmentLightTexture.Resource.Get());
				globalData.IsEnvironmentTextureCubeMap = IsCubeMap(m_scene->EnvironmentTexture.Resource.Get());
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

		ThrowIfFailed(device->CreateRootSignature(0, g_Raytracing_dxil, size(g_Raytracing_dxil), IID_PPV_ARGS(&m_rootSignature)));
	}

	void CreatePipelineStates() {
		const auto device = m_deviceResources->GetD3DDevice();

		{
			const CD3DX12_SHADER_BYTECODE shaderByteCode(g_Raytracing_dxil, size(g_Raytracing_dxil));
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
			ignore = slSetD3DDevice(device);

			DXGI_ADAPTER_DESC adapterDesc;
			ThrowIfFailed(m_deviceResources->GetAdapter()->GetDesc(&adapterDesc));
			m_streamline = make_unique<Streamline>(0, adapterDesc.AdapterLuid, commandList);
		}

		m_denoisedComposition = make_unique<DenoisedComposition>(device);

		m_temporalAntiAliasing = make_unique<TemporalAntiAliasing>(device);

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

			m_alphaBlending = make_unique<SpriteBatch>(device, resourceUploadBatch, SpriteBatchPipelineStateDescription(RenderTargetState(m_deviceResources->GetBackBufferFormat(), m_deviceResources->GetDepthBufferFormat()), &CommonStates::NonPremultiplied));

			resourceUploadBatch.End(commandQueue).get();
		}
	}

	void CreateAccelerationStructures(bool updateOnly) {
		const auto device = m_deviceResources->GetD3DDevice();
		const auto commandQueue = m_deviceResources->GetCommandQueue();

		CommandList<ID3D12GraphicsCommandList4> commandList(device);

		const auto pCommandList = commandList.GetNative();

		vector<uint64_t> newBottomLevelAccelerationStructureIDs;

		{
			commandList.Begin();

			if (!updateOnly) {
				m_bottomLevelAccelerationStructureIDs = {};

				m_accelerationStructureManager = make_unique<DxAccelStructManager>(device);
				m_accelerationStructureManager->Initialize();
			}

			vector<vector<D3D12_RAYTRACING_GEOMETRY_DESC>> geometryDescs;
			vector<D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS> newBuildBottomLevelAccelerationStructureInputs;
			vector<shared_ptr<Mesh>> newMeshes;

			for (const auto& renderObject : m_scene->RenderObjects) {
				const auto& mesh = renderObject.Mesh;
				if (const auto [first, second] = m_bottomLevelAccelerationStructureIDs.try_emplace(mesh); second) {
					auto& _geometryDescs = geometryDescs.emplace_back(initializer_list{ CreateGeometryDesc<Mesh::VertexType, Mesh::IndexType>(mesh->Vertices.Get(), mesh->Indices.Get()) });
					newBuildBottomLevelAccelerationStructureInputs.emplace_back(D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS{
						.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL,
						.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION,
						.NumDescs = static_cast<UINT>(size(_geometryDescs)),
						.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY,
						.pGeometryDescs = data(_geometryDescs)
						});
					newMeshes.emplace_back(mesh);
				}
			}

			if (!empty(newBuildBottomLevelAccelerationStructureInputs)) {
				m_accelerationStructureManager->PopulateBuildCommandList(pCommandList, data(newBuildBottomLevelAccelerationStructureInputs), static_cast<uint32_t>(size(newBuildBottomLevelAccelerationStructureInputs)), newBottomLevelAccelerationStructureIDs);
				for (UINT i = 0; const auto & meshNode : newMeshes) m_bottomLevelAccelerationStructureIDs[meshNode] = newBottomLevelAccelerationStructureIDs[i++];
				m_accelerationStructureManager->PopulateUAVBarriersCommandList(pCommandList, newBottomLevelAccelerationStructureIDs);
				m_accelerationStructureManager->PopulateCompactionSizeCopiesCommandList(pCommandList, newBottomLevelAccelerationStructureIDs);
			}

			commandList.End(commandQueue).get();
		}

		{
			commandList.Begin();

			if (!empty(newBottomLevelAccelerationStructureIDs)) m_accelerationStructureManager->PopulateCompactionCommandList(pCommandList, newBottomLevelAccelerationStructureIDs);

			if (!updateOnly) {
				m_topLevelAccelerationStructure = make_unique<TopLevelAccelerationStructure>(device, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE);
			}
			vector<D3D12_RAYTRACING_INSTANCE_DESC> instanceDescs;
			instanceDescs.reserve(m_topLevelAccelerationStructure->GetDescCount());
			for (UINT objectIndex = 0; const auto & renderObject : m_scene->RenderObjects) {
				auto& instanceDesc = instanceDescs.emplace_back(D3D12_RAYTRACING_INSTANCE_DESC{
					.InstanceID = objectIndex,
					.InstanceMask = renderObject.IsVisible ? ~0u : 0,
					.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE | D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_FRONT_COUNTERCLOCKWISE,
					.AccelerationStructure = m_accelerationStructureManager->GetAccelStructGPUVA(m_bottomLevelAccelerationStructureIDs.at(renderObject.Mesh))
					});
				XMStoreFloat3x4(reinterpret_cast<XMFLOAT3X4*>(instanceDesc.Transform), Transform(*renderObject.Shape));
				objectIndex++;
			}
			m_topLevelAccelerationStructure->Build(pCommandList, instanceDescs, updateOnly);

			commandList.End(commandQueue).get();
		}

		if (!empty(newBottomLevelAccelerationStructureIDs)) m_accelerationStructureManager->GarbageCollection(newBottomLevelAccelerationStructureIDs);
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
				.OutColor = ResourceDescriptorHeapIndex::OutColor,
				.OutLinearDepth = ResourceDescriptorHeapIndex::OutLinearDepth,
				.OutNormalizedDepth = ResourceDescriptorHeapIndex::OutNormalizedDepth,
				.OutMotionVectors = ResourceDescriptorHeapIndex::OutMotionVectors,
				.OutBaseColorMetalness = ResourceDescriptorHeapIndex::OutBaseColorMetalness,
				.OutEmissiveColor = ResourceDescriptorHeapIndex::OutEmissiveColor,
				.OutNormalRoughness = ResourceDescriptorHeapIndex::OutNormalRoughness,
				.OutNoisyDiffuse = ResourceDescriptorHeapIndex::OutNoisyDiffuse,
				.OutNoisySpecular = ResourceDescriptorHeapIndex::OutNoisySpecular
			}
		);
		CreateBuffer(m_GPUBuffers.GraphicsSettings, GraphicsSettings(), ResourceDescriptorHeapIndex::InGraphicsSettings);
		CreateBuffer(m_GPUBuffers.Camera, Camera{ .IsNormalizedDepthReversed = true }, ResourceDescriptorHeapIndex::InCamera);
		CreateBuffer(m_GPUBuffers.SceneData, SceneData(), ResourceDescriptorHeapIndex::InSceneData);
	}

	void CreateStructuredBuffers() {
		if (const auto objectCount = static_cast<UINT>(size(m_scene->RenderObjects))) {
			const auto CreateBuffer = [&]<typename T>(unique_ptr<T>&uploadBuffer, UINT count, UINT descriptorHeapIndex) {
				uploadBuffer = make_unique<T>(m_deviceResources->GetD3DDevice(), count);
				uploadBuffer->CreateShaderResourceView(m_resourceDescriptorHeap->GetCpuHandle(descriptorHeapIndex));
			};
			CreateBuffer(m_GPUBuffers.InstanceData, m_topLevelAccelerationStructure->GetDescCount(), ResourceDescriptorHeapIndex::InInstanceData);
			CreateBuffer(m_GPUBuffers.ObjectResourceDescriptorHeapIndices, objectCount, ResourceDescriptorHeapIndex::InObjectResourceDescriptorHeapIndices);
			CreateBuffer(m_GPUBuffers.ObjectData, objectCount, ResourceDescriptorHeapIndex::InObjectData);

			for (UINT instanceIndex = 0, objectIndex = 0; const auto & renderObject : m_scene->RenderObjects) {
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
					const auto index = Texture.DescriptorHeapIndices.SRV;
					switch (auto& indices = objectResourceDescriptorHeapIndices.Textures; TextureType) {
						case TextureType::BaseColorMap: indices.BaseColorMap = index; break;
						case TextureType::EmissiveColorMap: indices.EmissiveColorMap = index; break;
						case TextureType::MetallicMap: indices.MetallicMap = index; break;
						case TextureType::RoughnessMap: indices.RoughnessMap = index; break;
						case TextureType::AmbientOcclusionMap: indices.AmbientOcclusionMap = index; break;
						case TextureType::TransmissionMap: indices.TransmissionMap = index; break;
						case TextureType::OpacityMap: indices.OpacityMap = index; break;
						case TextureType::NormalMap: indices.NormalMap = index; break;
						default: throw out_of_range("Unsupported texture type");
					}
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

			const auto rotationSpeed = XM_2PI * 4e-4f * speedSettings.Rotation;
			yaw += static_cast<float>(mouseState.x) * rotationSpeed;
			pitch += static_cast<float>(-mouseState.y) * rotationSpeed;
		}

		if (pitch == 0) {
			if (displacement == Vector3() && yaw == 0) return;
		}
		else if (const auto angle = XM_PIDIV2 - abs(asin(m_cameraController.GetNormalizedForwardDirection().y) + pitch); angle <= 0) pitch = copysign(max(0.0f, angle - 0.1f), pitch);

		m_cameraController.Move(m_cameraController.GetNormalizedRightDirection() * displacement.x + m_cameraController.GetNormalizedUpDirection() * displacement.y + m_cameraController.GetNormalizedForwardDirection() * displacement.z);
		m_cameraController.Rotate(yaw, pitch);
	}

	void UpdateGraphicsSettings() {
		const auto& raytracingSettings = g_graphicsSettings.Raytracing;
		m_GPUBuffers.GraphicsSettings->GetData() = {
			.FrameIndex = m_stepTimer.GetFrameCount() - 1,
			.MaxNumberOfBounces = raytracingSettings.MaxNumberOfBounces,
			.SamplesPerPixel = raytracingSettings.SamplesPerPixel,
			.IsRussianRouletteEnabled = raytracingSettings.IsRussianRouletteEnabled,
			.NRDDenoiser = g_graphicsSettings.PostProcessing.NRD.Denoiser,
			.NRDHitDistanceParameters = reinterpret_cast<const XMFLOAT4&>(m_NRDReblurSettings.hitDistanceParameters)
		};
	}

	void UpdateScene() {
		if (!m_scene->IsStatic()) {
			for (UINT instanceIndex = 0; const auto & renderObject : m_scene->RenderObjects) m_GPUBuffers.InstanceData->GetData(instanceIndex++).PreviousObjectToWorld = Transform(*renderObject.Shape);
		}

		m_scene->Tick(static_cast<PxReal>(min(1.0 / 60, m_stepTimer.GetElapsedSeconds())), m_inputDeviceStateTrackers.Gamepad, m_inputDeviceStateTrackers.Keyboard, m_inputDeviceStateTrackers.Mouse);

		{
			auto& sceneData = m_GPUBuffers.SceneData->GetData();
			sceneData.IsStatic = m_scene->IsStatic();
			sceneData.EnvironmentLightColor = m_scene->EnvironmentLightColor;
			sceneData.EnvironmentLightTextureTransform = m_scene->EnvironmentLightTexture.Transform();
			sceneData.EnvironmentColor = m_scene->EnvironmentColor;
			sceneData.EnvironmentTextureTransform = m_scene->EnvironmentTexture.Transform();
		}
	}

	void DispatchRays() {
		const auto commandList = m_deviceResources->GetCommandList();
		commandList->SetComputeRootSignature(m_rootSignature.Get());
		commandList->SetComputeRootShaderResourceView(0, m_topLevelAccelerationStructure->GetBuffer()->GetGPUVirtualAddress());
		commandList->SetComputeRoot32BitConstants(1, 2, &m_renderSize, 0);
		commandList->SetComputeRootConstantBufferView(2, m_GPUBuffers.GlobalResourceDescriptorHeapIndices->GetResource()->GetGPUVirtualAddress());
		commandList->SetPipelineState(m_pipelineState.Get());
		commandList->Dispatch((m_renderSize.x + 15) / 16, (m_renderSize.y + 15) / 16, 1);
	}

	auto CreateResourceTagInfo(BufferType type, const RenderTexture& texture, bool isRenderSize = true, ResourceLifecycle lifecycle = ResourceLifecycle::eValidUntilEvaluate) const {
		ResourceTagInfo resourceTagInfo{
			.Type = type,
			.Resource = Resource(sl::ResourceType::eTex2d, texture.GetResource(), texture.GetState()),
			.Lifecycle = lifecycle
		};
		if (isRenderSize) {
			resourceTagInfo.Extent = { .width = m_renderSize.x, .height = m_renderSize.y };
		}
		return resourceTagInfo;
	}

	bool IsDLSSSuperResolutionEnabled() const { return m_streamline->IsFeatureAvailable(kFeatureDLSS) && g_graphicsSettings.PostProcessing.DLSS.SuperResolutionMode != DLSSSuperResolutionMode::Off; }
	bool IsNISEnabled() const { return g_graphicsSettings.PostProcessing.NIS.IsEnabled && m_streamline->IsFeatureAvailable(kFeatureNIS); }
	bool IsNRDEnabled() const { return g_graphicsSettings.PostProcessing.NRD.Denoiser != NRDDenoiser::None && m_NRD->IsAvailable(); }

	void SetDLSSOptimalSettings() {
		switch (auto& mode = m_DLSSOptions.mode; g_graphicsSettings.PostProcessing.DLSS.SuperResolutionMode) {
			case DLSSSuperResolutionMode::Auto:
			{
				const auto outputSize = GetOutputSize();
				if (const auto minValue = min(outputSize.cx, outputSize.cy);
					minValue <= 720) mode = DLSSMode::eDLAA;
				else if (minValue <= 1440) mode = DLSSMode::eMaxQuality;
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

		DLSSOptimalSettings optimalSettings;
		slDLSSGetOptimalSettings(m_DLSSOptions, optimalSettings);
		ignore = m_streamline->SetConstants(m_DLSSOptions);
		m_renderSize = XMUINT2(optimalSettings.optimalRenderWidth, optimalSettings.optimalRenderHeight);
	}

	void ResetTemporalAccumulation() {
		m_slConstants.reset = Boolean::eTrue;

		m_NRDCommonSettings.accumulationMode = AccumulationMode::CLEAR_AND_RESTART;

		m_temporalAntiAliasing->GetData().Reset = true;
	}

	void OnRenderSizeChanged() {
		const auto outputSize = GetOutputSize();
		m_haltonSamplePattern = static_cast<UINT>(8 * (static_cast<float>(outputSize.cx) / static_cast<float>(m_renderSize.x)) * (static_cast<float>(outputSize.cy) / static_cast<float>(m_renderSize.y)));

		ResetTemporalAccumulation();
	}

	void PostProcessGraphics() {
		const auto& postProcessingSettings = g_graphicsSettings.PostProcessing;

		PrepareStreamline();

		const auto isNRDEnabled = IsNRDEnabled() && postProcessingSettings.NRD.SplitScreen != 1;

		if (isNRDEnabled) ProcessNRD();

		auto inColor = m_renderTextures.at(RenderTextureNames::Color).get(), outColor = m_renderTextures.at(RenderTextureNames::FinalColor).get();

		if (IsDLSSSuperResolutionEnabled()) {
			ProcessDLSSSuperResolution();

			swap(inColor, outColor);
		}
		else if (postProcessingSettings.IsTemporalAntiAliasingEnabled) {
			ProcessTemporalAntiAliasing();

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

		ProcessToneMapping(*inColor);

		if (isNRDEnabled && postProcessingSettings.NRD.IsValidationOverlayEnabled) ProcessAlphaBlending(*m_renderTextures.at(RenderTextureNames::Validation));
	}

	void PrepareStreamline() {
		ignore = m_streamline->NewFrame();

		const auto& camera = m_GPUBuffers.Camera->GetData();
		m_slConstants.jitterOffset = { -camera.PixelJitter.x, -camera.PixelJitter.y };
		reinterpret_cast<XMFLOAT3&>(m_slConstants.cameraPos) = m_cameraController.GetPosition();
		reinterpret_cast<XMFLOAT3&>(m_slConstants.cameraUp) = m_cameraController.GetUpDirection();
		reinterpret_cast<XMFLOAT3&>(m_slConstants.cameraRight) = m_cameraController.GetRightDirection();
		reinterpret_cast<XMFLOAT3&>(m_slConstants.cameraFwd) = m_cameraController.GetForwardDirection();
		m_slConstants.cameraNear = m_cameraController.GetNearDepth();
		m_slConstants.cameraFar = m_cameraController.GetFarDepth();
		m_slConstants.cameraFOV = m_cameraController.GetHorizontalFieldOfView();
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

		const auto& camera = m_GPUBuffers.Camera->GetData();

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

			reinterpret_cast<XMFLOAT4X4&>(m_NRDCommonSettings.worldToViewMatrixPrev) = camera.PreviousWorldToView;
			reinterpret_cast<XMFLOAT4X4&>(m_NRDCommonSettings.viewToClipMatrixPrev) = camera.PreviousViewToProjection;
			reinterpret_cast<XMFLOAT4X4&>(m_NRDCommonSettings.worldToViewMatrix) = m_cameraController.GetWorldToView();
			reinterpret_cast<XMFLOAT4X4&>(m_NRDCommonSettings.viewToClipMatrix) = m_cameraController.GetViewToProjection();
			ranges::copy(m_NRDCommonSettings.cameraJitter, m_NRDCommonSettings.cameraJitterPrev);
			reinterpret_cast<XMFLOAT2&>(m_NRDCommonSettings.cameraJitter) = camera.PixelJitter;

			const auto outputSize = GetOutputSize();
			ranges::copy(m_NRDCommonSettings.resolutionScale, m_NRDCommonSettings.resolutionScalePrev);
			reinterpret_cast<XMFLOAT2&>(m_NRDCommonSettings.resolutionScale) = { static_cast<float>(m_renderSize.x) / static_cast<float>(outputSize.cx), static_cast<float>(m_renderSize.y) / static_cast<float>(outputSize.cy) };
			reinterpret_cast<XMFLOAT3&>(m_NRDCommonSettings.motionVectorScale) = { 1 / static_cast<float>(m_renderSize.x), 1 / static_cast<float>(m_renderSize.y), 1 };

			m_NRDCommonSettings.splitScreen = NRDSettings.SplitScreen;
			m_NRDCommonSettings.enableValidation = NRDSettings.IsValidationOverlayEnabled;

			ignore = m_NRD->SetCommonSettings(m_NRDCommonSettings);

			const auto denoiser = static_cast<Identifier>(NRDSettings.Denoiser);
			if (NRDSettings.Denoiser == NRDDenoiser::ReBLUR) ignore = m_NRD->SetDenoiserSettings(denoiser, m_NRDReblurSettings);
			else if (NRDSettings.Denoiser == NRDDenoiser::ReLAX) ignore = m_NRD->SetDenoiserSettings(denoiser, m_NRDRelaxSettings);
			m_NRD->Denoise(initializer_list<Identifier>{ denoiser });

			m_NRDCommonSettings.accumulationMode = AccumulationMode::CONTINUE;
		}

		const auto descriptorHeap = m_resourceDescriptorHeap->Heap();
		commandList->SetDescriptorHeaps(1, &descriptorHeap);

		{
			const auto& emissiveColor = *m_renderTextures.at(RenderTextureNames::EmissiveColor);

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

			m_denoisedComposition->Descriptors = {
				.InLinearDepth = m_resourceDescriptorHeap->GetGpuHandle(linearDepth.GetSrvDescriptorHeapIndex()),
				.InBaseColorMetalness = m_resourceDescriptorHeap->GetGpuHandle(baseColorMetalness.GetSrvDescriptorHeapIndex()),
				.InEmissiveColor = m_resourceDescriptorHeap->GetGpuHandle(emissiveColor.GetSrvDescriptorHeapIndex()),
				.InNormalRoughness = m_resourceDescriptorHeap->GetGpuHandle(normalRoughness.GetSrvDescriptorHeapIndex()),
				.InDenoisedDiffuse = m_resourceDescriptorHeap->GetGpuHandle(denoisedDiffuse.GetSrvDescriptorHeapIndex()),
				.InDenoisedSpecular = m_resourceDescriptorHeap->GetGpuHandle(denoisedSpecular.GetSrvDescriptorHeapIndex()),
				.OutColor = m_resourceDescriptorHeap->GetGpuHandle(m_renderTextures.at(RenderTextureNames::Color)->GetUavDescriptorHeapIndex())
			};

			m_denoisedComposition->RenderSize = m_renderSize;

			m_denoisedComposition->GetData() = {
				.NRDDenoiser = NRDSettings.Denoiser,
				.CameraRightDirection = m_cameraController.GetRightDirection(),
				.CameraUpDirection = m_cameraController.GetUpDirection(),
				.CameraForwardDirection = m_cameraController.GetForwardDirection(),
				.CameraPixelJitter = camera.PixelJitter
			};

			m_denoisedComposition->Process(commandList);
		}
	}

	void ProcessTemporalAntiAliasing() {
		const auto commandList = m_deviceResources->GetCommandList();

		const auto& historyColor = *m_renderTextures.at(RenderTextureNames::HistoryColor), & finalColor = *m_renderTextures.at(RenderTextureNames::FinalColor);

		{
			const auto& color = *m_renderTextures.at(RenderTextureNames::Color), & motionVectors = *m_renderTextures.at(RenderTextureNames::MotionVectors);

			const ScopedBarrier scopedBarrier(
				commandList,
				{
					CD3DX12_RESOURCE_BARRIER::Transition(historyColor.GetResource(), historyColor.GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(color.GetResource(), color.GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(motionVectors.GetResource(), motionVectors.GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE)
				}
			);

			m_temporalAntiAliasing->Descriptors = {
				.InHistoryColor = m_resourceDescriptorHeap->GetGpuHandle(historyColor.GetSrvDescriptorHeapIndex()),
				.InCurrentColor = m_resourceDescriptorHeap->GetGpuHandle(color.GetSrvDescriptorHeapIndex()),
				.InMotionVectors = m_resourceDescriptorHeap->GetGpuHandle(motionVectors.GetSrvDescriptorHeapIndex()),
				.OutFinalColor = m_resourceDescriptorHeap->GetGpuHandle(finalColor.GetUavDescriptorHeapIndex())
			};

			m_temporalAntiAliasing->RenderSize = m_renderSize;

			auto& data = m_temporalAntiAliasing->GetData();

			data.CameraPosition = m_cameraController.GetPosition();
			data.CameraRightDirection = m_cameraController.GetRightDirection();
			data.CameraUpDirection = m_cameraController.GetUpDirection();
			data.CameraForwardDirection = m_cameraController.GetForwardDirection();
			data.CameraPreviousWorldToProjection = m_GPUBuffers.Camera->GetData().PreviousWorldToProjection;

			m_temporalAntiAliasing->Process(commandList);

			data.Reset = false;
		}

		{
			const ScopedBarrier scopedBarrier(
				commandList,
				{
					CD3DX12_RESOURCE_BARRIER::Transition(historyColor.GetResource(), historyColor.GetState(), D3D12_RESOURCE_STATE_COPY_DEST),
					CD3DX12_RESOURCE_BARRIER::Transition(finalColor.GetResource(), finalColor.GetState(), D3D12_RESOURCE_STATE_COPY_SOURCE)
				}
			);

			commandList->CopyResource(historyColor.GetResource(), finalColor.GetResource());
		}
	}

	void ProcessDLSSSuperResolution() {
		const auto commandList = m_deviceResources->GetCommandList();

		ResourceTagInfo resourceTagInfos[]{
			CreateResourceTagInfo(kBufferTypeDepth, *m_renderTextures.at(RenderTextureNames::NormalizedDepth)),
			CreateResourceTagInfo(kBufferTypeMotionVectors, *m_renderTextures.at(RenderTextureNames::MotionVectors)),
			CreateResourceTagInfo(kBufferTypeScalingInputColor, *m_renderTextures.at(RenderTextureNames::Color)),
			CreateResourceTagInfo(kBufferTypeScalingOutputColor, *m_renderTextures.at(RenderTextureNames::FinalColor), false)
		};
		ignore = m_streamline->EvaluateFeature(kFeatureDLSS, CreateResourceTags(resourceTagInfos));

		const auto descriptorHeap = m_resourceDescriptorHeap->Heap();
		commandList->SetDescriptorHeaps(1, &descriptorHeap);
	}

	void ProcessNIS(RenderTexture& inColor, RenderTexture& outColor) {
		const auto commandList = m_deviceResources->GetCommandList();

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
		commandList->SetDescriptorHeaps(1, &descriptorHeap);
	}

	void ProcessChromaticAberration(RenderTexture& inColor, RenderTexture& outColor) {
		const auto commandList = m_deviceResources->GetCommandList();

		const ScopedBarrier scopedBarrier(commandList, { CD3DX12_RESOURCE_BARRIER::Transition(inColor.GetResource(), inColor.GetState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE) });

		m_chromaticAberration->Descriptors = {
			.InColor = m_resourceDescriptorHeap->GetGpuHandle(inColor.GetSrvDescriptorHeapIndex()),
			.OutColor = m_resourceDescriptorHeap->GetGpuHandle(outColor.GetUavDescriptorHeapIndex())
		};

		const auto outputSize = GetOutputSize();
		m_chromaticAberration->RenderSize = XMUINT2(outputSize.cx, outputSize.cy);

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
				{
					auto isChanged = false;

					if (ImGui::BeginCombo("Window Mode", ToString(g_graphicsSettings.WindowMode))) {
						for (const auto windowMode : { WindowMode::Windowed, WindowMode::Borderless, WindowMode::Fullscreen }) {
							const auto isSelected = g_graphicsSettings.WindowMode == windowMode;

							if (ImGui::Selectable(ToString(windowMode), isSelected)) {
								g_graphicsSettings.WindowMode = windowMode;

								m_windowModeHelper->SetMode(windowMode);

								isChanged = true;
							}

							if (isSelected) ImGui::SetItemDefaultFocus();
						}

						ImGui::EndCombo();
					}

					if (const auto ToString = [](SIZE value) { return format("{} × {}", value.cx, value.cy); };
						ImGui::BeginCombo("Resolution", ToString(g_graphicsSettings.Resolution).c_str())) {
						for (const auto& displayResolution : g_displayResolutions) {
							const auto isSelected = g_graphicsSettings.Resolution == displayResolution;

							if (ImGui::Selectable(ToString(displayResolution).c_str(), isSelected)) {
								g_graphicsSettings.Resolution = displayResolution;

								m_windowModeHelper->SetResolution(displayResolution);

								isChanged = true;
							}

							if (isSelected) ImGui::SetItemDefaultFocus();
						}

						ImGui::EndCombo();
					}

					if (isChanged) m_futures["WindowSetting"] = async(launch::deferred, [&] { ThrowIfFailed(m_windowModeHelper->Apply()); });
				}

				{
					const ImGuiEx::ScopedEnablement scopedEnablement(m_deviceResources->GetDeviceOptions() & DeviceResources::c_AllowTearing);

					if (auto isEnabled = m_deviceResources->IsVSyncEnabled(); ImGui::Checkbox("V-Sync", &isEnabled) && m_deviceResources->EnableVSync(isEnabled)) g_graphicsSettings.IsVSyncEnabled = isEnabled;
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

					isChanged |= ImGui::SliderInt("Max Number of Bounces", reinterpret_cast<int*>(&raytracingSettings.MaxNumberOfBounces), 1, raytracingSettings.MaxMaxNumberOfBounces, "%d", ImGuiSliderFlags_AlwaysClamp);

					isChanged |= ImGui::SliderInt("Samples Per Pixel", reinterpret_cast<int*>(&raytracingSettings.SamplesPerPixel), 1, raytracingSettings.MaxSamplesPerPixel, "%d", ImGuiSliderFlags_AlwaysClamp);

					if (isChanged) ResetTemporalAccumulation();

					ImGui::TreePop();
				}

				if (ImGui::TreeNodeEx("Post-Processing", ImGuiTreeNodeFlags_DefaultOpen)) {
					auto& postProcessingSetttings = g_graphicsSettings.PostProcessing;

					{
						const ImGuiEx::ScopedEnablement scopedEnablement(m_NRD->IsAvailable());

						if (ImGui::TreeNodeEx("NVIDIA Real-Time Denoisers", ImGuiTreeNodeFlags_DefaultOpen)) {
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

							if (NRDSettings.Denoiser != NRDDenoiser::None) {
								ImGui::Checkbox("Validation Overlay", &NRDSettings.IsValidationOverlayEnabled);

								ImGui::SliderFloat("Split Screen", &NRDSettings.SplitScreen, 0, 1, "%.2f", ImGuiSliderFlags_AlwaysClamp);
							}

							ImGui::TreePop();
						}
					}

					if (!IsDLSSSuperResolutionEnabled() && ImGui::Checkbox("Temporal Anti-Aliasing", &postProcessingSetttings.IsTemporalAntiAliasingEnabled)) ResetTemporalAccumulation();

					{
						const ImGuiEx::ScopedEnablement scopedEnablement(m_streamline->IsFeatureAvailable(kFeatureDLSS));

						if (ImGui::TreeNodeEx("NVIDIA DLSS", ImGuiTreeNodeFlags_DefaultOpen)) {
							auto& DLSSSettings = postProcessingSetttings.DLSS;

							auto isChanged = false;

							if (ImGui::BeginCombo("Super Resolution", ToString(DLSSSettings.SuperResolutionMode))) {
								for (const auto DLSSSuperResolutionMode : { DLSSSuperResolutionMode::Off, DLSSSuperResolutionMode::Auto, DLSSSuperResolutionMode::DLAA, DLSSSuperResolutionMode::Quality, DLSSSuperResolutionMode::Balanced, DLSSSuperResolutionMode::Performance, DLSSSuperResolutionMode::UltraPerformance }) {
									const auto isSelected = DLSSSettings.SuperResolutionMode == DLSSSuperResolutionMode;

									if (ImGui::Selectable(ToString(DLSSSuperResolutionMode), isSelected)) {
										DLSSSettings.SuperResolutionMode = DLSSSuperResolutionMode;

										isChanged = true;
									}

									if (isSelected) ImGui::SetItemDefaultFocus();
								}

								ImGui::EndCombo();
							}

							if (isChanged) {
								if (DLSSSettings.SuperResolutionMode != DLSSSuperResolutionMode::Off) SetDLSSOptimalSettings();
								else {
									const auto [Width, Height] = GetOutputSize();
									m_renderSize = XMUINT2(Width, Height);
								}

								OnRenderSizeChanged();
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
								const ImGuiEx::ScopedID scopedID("Enable NVIDIA Image Scaling");

								if (ImGui::Checkbox("Enable", &isEnabled)) NISSettings.IsEnabled = isEnabled;
							}

							if (isEnabled) ImGui::SliderFloat("Sharpness", &NISSettings.Sharpness, 0, 1, "%.2f", ImGuiSliderFlags_AlwaysClamp);

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
							for (const auto toneMappingOperator : { ToneMapPostProcess::None, ToneMapPostProcess::Saturate, ToneMapPostProcess::Reinhard, ToneMapPostProcess::ACESFilmic }) {
								const auto isSelected = toneMappingSettings.Operator == toneMappingOperator;

								if (ImGui::Selectable(ToString(toneMappingOperator), isSelected)) toneMappingSettings.Operator = toneMappingOperator;

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
