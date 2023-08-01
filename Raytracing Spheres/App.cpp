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

#include "directxtk12/ResourceUploadBatch.h"

#include "directxtk12/CommonStates.h"

#include "directxtk12/SpriteBatch.h"

#include "directxtk12/PostProcess.h"

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
import DirectX.CommandList;
import DirectX.DescriptorHeap;
import DirectX.PostProcess.DenoisedComposition;
import DirectX.PostProcess.TemporalAntiAliasing;
import DirectX.RaytracingHelpers;
import HaltonSamplePattern;
import Model;
import MyScene;
import NRD;
import RaytracingShaderData;
import Scene;
import SharedData;
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

		m_temporalAntiAliasing.reset();

		m_denoisedComposition.reset();
		m_NRD.reset();

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

	struct FutureNames {
		MAKE_NAME(Scene);
	};
	map<string, future<void>, less<>> m_futures;

	mutex m_exceptionMutex;
	exception_ptr m_exception;

	struct ResourceDescriptorHeapIndex {
		enum {
			GraphicsSettings,
			Camera,
			SceneData,
			InstanceData,
			ObjectResourceDescriptorHeapIndices, ObjectData,
			MotionsSRV, MotionsUAV,
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
			Blur1, Blur2,
			Font,
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

	CommonSettings m_NRDCommonSettings{ .isBaseColorMetalnessAvailable = true };
	ReblurSettings m_NRDReblurSettings{ .hitDistanceReconstructionMode = HitDistanceReconstructionMode::AREA_5X5, .enableAntiFirefly = true };
	unique_ptr<NRD> m_NRD;
	unique_ptr<DenoisedComposition> m_denoisedComposition;

	unique_ptr<TemporalAntiAliasing> m_temporalAntiAliasing;

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
		MAKE_NAME(Motions);
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

	struct { bool IsVisible, HasFocus = true, IsSettingsVisible; } m_UIStates{};

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

			if (m_NRD = make_unique<NRD>(device, m_deviceResources->GetCommandQueue(), m_deviceResources->GetCommandList(), m_deviceResources->GetBackBufferCount(), initializer_list{ Method::REBLUR_DIFFUSE_SPECULAR }, outputSize);
				m_NRD->IsAvailable()) {
				const auto CreateTexture1 = [&](DXGI_FORMAT format, LPCSTR textureName, ResourceType resourceType, UINT srvDescriptorHeapIndex = ~0u, UINT uavDescriptorHeapIndex = ~0u) {
					CreateTexture(format, outputSize, textureName, srvDescriptorHeapIndex, uavDescriptorHeapIndex);

					const auto& texture = m_renderTextures.at(textureName);
					m_NRD->SetResource(resourceType, texture->GetResource(), texture->GetCurrentState());
				};

				CreateTexture1(DXGI_FORMAT_R16G16B16A16_FLOAT, RenderTextureNames::Motions, ResourceType::IN_MV, ResourceDescriptorHeapIndex::MotionsSRV, ResourceDescriptorHeapIndex::MotionsUAV);

				CreateTexture1(NRD::ToDXGIFormat(GetLibraryDesc().normalEncoding), RenderTextureNames::NormalRoughness, ResourceType::IN_NORMAL_ROUGHNESS, ResourceDescriptorHeapIndex::NormalRoughnessSRV, ResourceDescriptorHeapIndex::NormalRoughnessUAV);

				CreateTexture1(DXGI_FORMAT_R32_FLOAT, RenderTextureNames::ViewZ, ResourceType::IN_VIEWZ, ResourceDescriptorHeapIndex::ViewZSRV, ResourceDescriptorHeapIndex::ViewZUAV);

				CreateTexture1(DXGI_FORMAT_R8G8B8A8_UNORM, RenderTextureNames::BaseColorMetalness, ResourceType::IN_BASECOLOR_METALNESS, ResourceDescriptorHeapIndex::BaseColorMetalnessSRV, ResourceDescriptorHeapIndex::BaseColorMetalnessUAV);

				CreateTexture1(DXGI_FORMAT_R16G16B16A16_FLOAT, RenderTextureNames::NoisyDiffuse, ResourceType::IN_DIFF_RADIANCE_HITDIST, ~0u, ResourceDescriptorHeapIndex::NoisyDiffuse);
				CreateTexture1(DXGI_FORMAT_R16G16B16A16_FLOAT, RenderTextureNames::NoisySpecular, ResourceType::IN_SPEC_RADIANCE_HITDIST, ~0u, ResourceDescriptorHeapIndex::NoisySpecular);

				CreateTexture1(DXGI_FORMAT_R16G16B16A16_FLOAT, RenderTextureNames::DenoisedDiffuse, ResourceType::OUT_DIFF_RADIANCE_HITDIST, ResourceDescriptorHeapIndex::DenoisedDiffuseSRV, ResourceDescriptorHeapIndex::DenoisedDiffuseUAV);
				CreateTexture1(DXGI_FORMAT_R16G16B16A16_FLOAT, RenderTextureNames::DenoisedSpecular, ResourceType::OUT_SPEC_RADIANCE_HITDIST, ResourceDescriptorHeapIndex::DenoisedSpecularSRV, ResourceDescriptorHeapIndex::DenoisedSpecularUAV);

				CreateTexture1(DXGI_FORMAT_R8G8B8A8_UNORM, RenderTextureNames::Validation, ResourceType::OUT_VALIDATION, ResourceDescriptorHeapIndex::ValidationSRV, ResourceDescriptorHeapIndex::ValidationUAV);

				reinterpret_cast<XMFLOAT3&>(m_NRDCommonSettings.motionVectorScale) = { 1 / static_cast<float>(outputSize.cx), 1 / static_cast<float>(outputSize.cy), 1 };

				m_NRD->SetMethodSettings(Method::REBLUR_DIFFUSE_SPECULAR, &m_NRDReblurSettings);
			}

			{
				CreateTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, outputSize, RenderTextureNames::HistoryOutput, ResourceDescriptorHeapIndex::HistoryOutputSRV, ResourceDescriptorHeapIndex::HistoryOutputUAV);
				CreateTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, outputSize, RenderTextureNames::Output, ResourceDescriptorHeapIndex::OutputSRV, ResourceDescriptorHeapIndex::OutputUAV, RenderDescriptorHeapIndex::Output);
				CreateTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, outputSize, RenderTextureNames::FinalOutput, ResourceDescriptorHeapIndex::FinalOutputSRV, ResourceDescriptorHeapIndex::FinalOutputUAV, RenderDescriptorHeapIndex::FinalOutput);
			}

			{
				const SIZE size{ outputSize.cx / 2, outputSize.cy / 2 };
				CreateTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, size, RenderTextureNames::Blur1, ResourceDescriptorHeapIndex::Blur1, ~0u, RenderDescriptorHeapIndex::Blur1);
				CreateTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, size, RenderTextureNames::Blur2, ResourceDescriptorHeapIndex::Blur2, ~0u, RenderDescriptorHeapIndex::Blur2);
			}

			m_denoisedComposition->TextureSize = m_temporalAntiAliasing->TextureSize = outputSize;

			m_alphaBlending->SetViewport(m_deviceResources->GetScreenViewport());
		}

		m_cameraController.SetLens(XMConvertToRadians(g_graphicsSettings.Camera.VerticalFieldOfView), static_cast<float>(outputSize.cx) / static_cast<float>(outputSize.cy));

		m_haltonSamplePattern.Reset();

		{
			const auto& IO = ImGui::GetIO();

			if (IO.BackendRendererUserData != nullptr) ImGui_ImplDX12_Shutdown();
			ImGui_ImplDX12_Init(device, static_cast<int>(m_deviceResources->GetBackBufferCount()), m_deviceResources->GetBackBufferFormat(), m_resourceDescriptorHeap->Heap(), m_resourceDescriptorHeap->GetCpuHandle(ResourceDescriptorHeapIndex::Font), m_resourceDescriptorHeap->GetGpuHandle(ResourceDescriptorHeapIndex::Font));

			IO.Fonts->Clear();
			IO.Fonts->AddFontFromFileTTF(R"(C:\Windows\Fonts\segoeui.ttf)", static_cast<float>(outputSize.cy) * 0.022f);
		}
	}

	void Update() {
		PIXBeginEvent(PIX_COLOR_DEFAULT, L"Update");

		{
			auto& camera = m_GPUBuffers.Camera->GetData();

			camera.PixelJitter = reinterpret_cast<XMFLOAT2&>(m_NRDCommonSettings.cameraJitter) = g_graphicsSettings.Camera.IsJitterEnabled ? m_haltonSamplePattern.GetNext() : XMFLOAT2();

			camera.PreviousWorldToView = m_cameraController.GetWorldToView();
			camera.PreviousWorldToProjection = m_cameraController.GetWorldToProjection();

			reinterpret_cast<XMFLOAT4X4&>(m_NRDCommonSettings.worldToViewMatrixPrev) = m_cameraController.GetWorldToView();
			reinterpret_cast<XMFLOAT4X4&>(m_NRDCommonSettings.viewToClipMatrixPrev) = m_cameraController.GetViewToProjection();

			ProcessInput();

			camera.Position = m_cameraController.GetPosition();
			camera.RightDirection = m_cameraController.GetRightDirection();
			camera.UpDirection = m_cameraController.GetUpDirection();
			camera.ForwardDirection = m_cameraController.GetForwardDirection();
			camera.NearZ = m_cameraController.GetNearZ();
			camera.FarZ = m_cameraController.GetFarZ();

			reinterpret_cast<XMFLOAT4X4&>(m_NRDCommonSettings.worldToViewMatrix) = m_cameraController.GetWorldToView();
			reinterpret_cast<XMFLOAT4X4&>(m_NRDCommonSettings.viewToClipMatrix) = m_cameraController.GetViewToProjection();
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

			postProcessingSettings.RaytracingDenoising.SplitScreen = clamp(postProcessingSettings.RaytracingDenoising.SplitScreen, 0.0f, 1.0f);

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
			{
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

						CreateAccelerationStructures(commandList.GetPointer(), false);

						commandList.End(commandQueue).get();
					}

					{
						auto& globalResourceDescriptorHeapIndices = m_GPUBuffers.GlobalResourceDescriptorHeapIndices->GetData();
						globalResourceDescriptorHeapIndices.EnvironmentLightCubeMap = m_scene->EnvironmentLightCubeMap.Texture.DescriptorHeapIndices.SRV;
						globalResourceDescriptorHeapIndices.EnvironmentCubeMap = m_scene->EnvironmentCubeMap.Texture.DescriptorHeapIndices.SRV;
					}

					CreateStructuredBuffers();
				}
				catch (...) {
					const scoped_lock lock(m_exceptionMutex);

					if (!m_exception) m_exception = current_exception();
				}
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

		{
			m_denoisedComposition = make_unique<DenoisedComposition>(device);
			m_denoisedComposition->Descriptors = {
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
			m_temporalAntiAliasing->Descriptors = {
				.MotionsSRV = m_resourceDescriptorHeap->GetGpuHandle(ResourceDescriptorHeapIndex::MotionsSRV),
				.HistoryOutputSRV = m_resourceDescriptorHeap->GetGpuHandle(ResourceDescriptorHeapIndex::HistoryOutputSRV),
				.CurrentOutputSRV = m_resourceDescriptorHeap->GetGpuHandle(ResourceDescriptorHeapIndex::OutputSRV),
				.FinalOutputUAV = m_resourceDescriptorHeap->GetGpuHandle(ResourceDescriptorHeapIndex::FinalOutputUAV)
			};
		}

		{
			const RenderTargetState renderTargetState(DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_UNKNOWN);

			m_bloom = {
				.Extraction = make_unique<BasicPostProcess>(device, renderTargetState, BasicPostProcess::BloomExtract),
				.Blur = make_unique<BasicPostProcess>(device, renderTargetState, BasicPostProcess::BloomBlur),
				.Combination = make_unique<DualPostProcess>(device, renderTargetState, DualPostProcess::BloomCombine),
			};

			for (const auto toneMappingOperator : { ToneMapPostProcess::None, ToneMapPostProcess::Saturate, ToneMapPostProcess::Reinhard, ToneMapPostProcess::ACESFilmic }) {
				m_toneMapping[toneMappingOperator] = make_unique<ToneMapPostProcess>(device, RenderTargetState(m_deviceResources->GetBackBufferFormat(), DXGI_FORMAT_UNKNOWN), toneMappingOperator, toneMappingOperator == ToneMapPostProcess::None ? ToneMapPostProcess::Linear : ToneMapPostProcess::SRGB);
			}
		}

		{
			const RenderTargetState renderTargetState(m_deviceResources->GetBackBufferFormat(), m_deviceResources->GetDepthBufferFormat());

			ResourceUploadBatch resourceUploadBatch(device);
			resourceUploadBatch.Begin();

			m_alphaBlending = make_unique<SpriteBatch>(device, resourceUploadBatch, SpriteBatchPipelineStateDescription(renderTargetState, &CommonStates::NonPremultiplied));

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
				.GraphicsSettings = ResourceDescriptorHeapIndex::GraphicsSettings,
				.Camera = ResourceDescriptorHeapIndex::Camera,
				.SceneData = ResourceDescriptorHeapIndex::SceneData,
				.InstanceData = ResourceDescriptorHeapIndex::InstanceData,
				.ObjectResourceDescriptorHeapIndices = ResourceDescriptorHeapIndex::ObjectResourceDescriptorHeapIndices,
				.ObjectData = ResourceDescriptorHeapIndex::ObjectData,
				.Motions = ResourceDescriptorHeapIndex::MotionsUAV,
				.NormalRoughness = ResourceDescriptorHeapIndex::NormalRoughnessUAV,
				.ViewZ = ResourceDescriptorHeapIndex::ViewZUAV,
				.BaseColorMetalness = ResourceDescriptorHeapIndex::BaseColorMetalnessUAV,
				.NoisyDiffuse = ResourceDescriptorHeapIndex::NoisyDiffuse,
				.NoisySpecular = ResourceDescriptorHeapIndex::NoisySpecular,
				.Output = ResourceDescriptorHeapIndex::OutputUAV
			}
		);

		CreateBuffer(m_GPUBuffers.GraphicsSettings, GraphicsSettings(), ResourceDescriptorHeapIndex::GraphicsSettings);

		CreateBuffer(m_GPUBuffers.Camera, Camera(), ResourceDescriptorHeapIndex::Camera);

		CreateBuffer(m_GPUBuffers.SceneData, SceneData(), ResourceDescriptorHeapIndex::SceneData);
	}

	void CreateStructuredBuffers() {
		if (const UINT objectCount = static_cast<UINT>(size(m_scene->RenderObjects))) {
			const auto CreateBuffer = [&]<typename T>(unique_ptr<T>&uploadBuffer, UINT count, UINT descriptorHeapIndex) {
				uploadBuffer = make_unique<T>(m_deviceResources->GetD3DDevice(), count);
				uploadBuffer->CreateShaderResourceView(m_resourceDescriptorHeap->GetCpuHandle(descriptorHeapIndex));
			};

			CreateBuffer(m_GPUBuffers.InstanceData, m_topLevelAccelerationStructure->GetDescCount(), ResourceDescriptorHeapIndex::InstanceData);

			CreateBuffer(m_GPUBuffers.ObjectResourceDescriptorHeapIndices, objectCount, ResourceDescriptorHeapIndex::ObjectResourceDescriptorHeapIndices);

			CreateBuffer(m_GPUBuffers.ObjectData, objectCount, ResourceDescriptorHeapIndex::ObjectData);

			for (UINT instanceIndex = 0, objectIndex = 0, textureTransformIndex = 0; const auto & renderObject : m_scene->RenderObjects) {
				m_GPUBuffers.InstanceData->GetData(instanceIndex++).PreviousObjectToWorld = Transform(*renderObject.Shape);

				auto& objectData = m_GPUBuffers.ObjectData->GetData(objectIndex);

				objectData.Material = renderObject.Material;

				objectData.TextureTransform = renderObject.TextureTransform;

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

		if (isUIVisible != m_UIStates.IsVisible && !isUIVisible) {
			if (const auto e = ImGuiEx::FindLatestInputEvent(ImGui::GetCurrentContext(), ImGuiInputEventType_MousePos)) {
				auto& queue = ImGui::GetCurrentContext()->InputEventsQueue;
				queue[0] = *e;
				queue.shrink(1);
			}
		}

		if (auto& IO = ImGui::GetIO(); m_UIStates.IsVisible) {
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
			for (UINT instanceIndex = 0; const auto & renderObject : m_scene->RenderObjects) {
				m_GPUBuffers.InstanceData->GetData(instanceIndex++).PreviousObjectToWorld = Transform(*renderObject.Shape);
			}
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
		commandList->SetComputeRootConstantBufferView(1, m_GPUBuffers.GlobalResourceDescriptorHeapIndices->GetResource()->GetGPUVirtualAddress());

		commandList->SetPipelineState(m_pipelineState.Get());

		const auto outputSize = GetOutputSize();
		commandList->Dispatch(static_cast<UINT>((outputSize.cx + 16) / 16), static_cast<UINT>((outputSize.cy + 16) / 16), 1);
	}

	void PostProcessGraphics() {
		const auto commandList = m_deviceResources->GetCommandList();

		const auto& postProcessingSettings = g_graphicsSettings.PostProcessing;

		const auto isNRDEnabled = postProcessingSettings.RaytracingDenoising.IsEnabled && postProcessingSettings.RaytracingDenoising.SplitScreen != 1 && m_NRD->IsAvailable();

		if (isNRDEnabled) {
			Denoise();

			m_NRDCommonSettings.accumulationMode = AccumulationMode::CONTINUE;
		}
		else m_NRDCommonSettings.accumulationMode = AccumulationMode::RESTART;

		if (postProcessingSettings.IsTemporalAntiAliasingEnabled) ProcessTemporalAntiAliasing();

		auto
			input = m_renderTextures.at(postProcessingSettings.IsTemporalAntiAliasingEnabled ? RenderTextureNames::FinalOutput : RenderTextureNames::Output).get(),
			output = m_renderTextures.at(postProcessingSettings.IsTemporalAntiAliasingEnabled ? RenderTextureNames::Output : RenderTextureNames::FinalOutput).get();

		if (postProcessingSettings.Bloom.IsEnabled) {
			ProcessBloom(*input, *output);

			swap(input, output);
		}

		ProcessToneMapping(*input);

		if (isNRDEnabled && postProcessingSettings.RaytracingDenoising.IsValidationLayerEnabled) {
			const auto& validation = *m_renderTextures.at(RenderTextureNames::Validation);

			const ScopedBarrier scopedBarrier(commandList, { CD3DX12_RESOURCE_BARRIER::Transition(validation.GetResource(), validation.GetCurrentState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE) });

			m_alphaBlending->Begin(commandList);
			m_alphaBlending->Draw(m_resourceDescriptorHeap->GetGpuHandle(validation.GetSrvDescriptorHeapIndex()), GetTextureSize(validation.GetResource()), XMFLOAT2());
			m_alphaBlending->End();
		}
	}

	void Denoise() {
		const auto& raytracingDenoisingSettings = g_graphicsSettings.PostProcessing.RaytracingDenoising;
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
					CD3DX12_RESOURCE_BARRIER::Transition(normalRoughness.GetResource(), normalRoughness.GetCurrentState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(viewZ.GetResource(), viewZ.GetCurrentState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(baseColorMetalness.GetResource(), baseColorMetalness.GetCurrentState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(denoisedDiffuse.GetResource(), denoisedDiffuse.GetCurrentState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(denoisedSpecular.GetResource(), denoisedSpecular.GetCurrentState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE)
				}
			);

			m_denoisedComposition->GetData() = {
				.CameraPosition = m_cameraController.GetPosition(),
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

		const auto& historyOutput = *m_renderTextures.at(RenderTextureNames::HistoryOutput);

		{
			const auto& motions = *m_renderTextures.at(RenderTextureNames::Motions), & output = *m_renderTextures.at(RenderTextureNames::Output);

			const ScopedBarrier scopedBarrier(
				commandList,
				{
					CD3DX12_RESOURCE_BARRIER::Transition(motions.GetResource(), motions.GetCurrentState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(historyOutput.GetResource(), historyOutput.GetCurrentState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(output.GetResource(), output.GetCurrentState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE)
				}
			);

			m_temporalAntiAliasing->GetData() = {
				.CameraPosition = m_cameraController.GetPosition(),
				.CameraRightDirection = m_cameraController.GetRightDirection(),
				.CameraUpDirection = m_cameraController.GetUpDirection(),
				.CameraForwardDirection = m_cameraController.GetForwardDirection(),
				.CameraNearZ = m_cameraController.GetNearZ(),
				.PreviousWorldToProjection = m_GPUBuffers.Camera->GetData().PreviousWorldToProjection
			};

			m_temporalAntiAliasing->Process(commandList);
		}

		{
			const auto& finalOutput = *m_renderTextures.at(RenderTextureNames::FinalOutput);

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

		if (m_UIStates.IsSettingsVisible) RenderSettingsWindow();

		if (openPopupModalName != nullptr) RenderPopupModalWindow(openPopupModalName);

		if (m_futures.contains(FutureNames::Scene)) RenderLoadingSceneWindow();

		ImGui::Render();
		ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), m_deviceResources->GetCommandList());
	}

	LPCSTR RenderMenuBar() {
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

		return openPopupModalName;
	}

	void RenderSettingsWindow() {
		ImGui::SetNextWindowBgAlpha(g_UISettings.WindowOpacity);

		const auto& viewport = *ImGui::GetMainViewport();
		ImGui::SetNextWindowPos({ viewport.WorkPos.x, viewport.WorkPos.y });
		ImGui::SetNextWindowSize({});

		if (ImGui::Begin("Settings", &m_UIStates.IsSettingsVisible, ImGuiWindowFlags_HorizontalScrollbar)) {
			if (ImGui::TreeNode("Graphics")) {
				auto isChanged = false, isWindowSettingChanged = false;

				{
					const auto WindowModes = { "Windowed", "Borderless", "Fullscreen" };
					if (auto windowMode = m_windowModeHelper->GetMode();
						ImGui::Combo("Window Mode", reinterpret_cast<int*>(&windowMode), data(WindowModes), static_cast<int>(size(WindowModes)))) {
						m_windowModeHelper->SetMode(windowMode);

						g_graphicsSettings.WindowMode = windowMode;

						isChanged = isWindowSettingChanged = true;
					}
				}

				{
					const auto ToString = [](SIZE value) { return format("{} × {}", value.cx, value.cy); };
					if (const auto resolution = m_windowModeHelper->GetResolution();
						ImGui::BeginCombo("Resolution", ToString(resolution).c_str())) {
						for (const auto& displayResolution : g_displayResolutions) {
							const auto isSelected = resolution == displayResolution;

							if (ImGui::Selectable(ToString(displayResolution).c_str(), isSelected)) {
								m_windowModeHelper->SetResolution(displayResolution);

								g_graphicsSettings.Resolution = displayResolution;

								isChanged = isWindowSettingChanged = true;
							}

							if (isSelected) ImGui::SetItemDefaultFocus();
						}

						ImGui::EndCombo();
					}
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

					isChanged |= ImGui::SliderInt("Samples Per Pixel", reinterpret_cast<int*>(&raytracingSettings.SamplesPerPixel), 1, static_cast<int>(MaxSamplesPerPixel), "%d", ImGuiSliderFlags_AlwaysClamp);

					ImGui::TreePop();
				}

				if (ImGui::TreeNodeEx("Post-Processing", ImGuiTreeNodeFlags_DefaultOpen)) {
					auto& postProcessingSetttings = g_graphicsSettings.PostProcessing;

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

				if (isChanged) ignore = g_graphicsSettings.Save();

				if (isWindowSettingChanged) m_futures["WindowSetting"] = async(launch::deferred, [&] { ThrowIfFailed(m_windowModeHelper->Apply()); });
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

	void RenderPopupModalWindow(LPCSTR openPopupModalName) {
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
