#pragma once

#pragma warning(push)

#pragma warning(disable: 26812)

#include "pch.h"

#include "DeviceResources.h"

#include "StepTimer.h"

#include "DirectXTK12/GraphicsMemory.h"

#include "DirectXTK12/GamePad.h"
#include "DirectXTK12/Keyboard.h"
#include "DirectXTK12/Mouse.h"

#include "DirectXTK12/VertexTypes.h"

#include "ShaderBindingTableGenerator.h"

#include "DirectXRaytracingHelpers.h"

#include "TemporalAntiAliasingEffect.h"

#include "Material.h"
#include "Texture.h"

#include "Camera.h"

#include "MyPhysX.h"

#pragma warning(pop)

class D3DApp : public DX::IDeviceNotify {
public:
	D3DApp(const D3DApp&) = delete;
	D3DApp& operator=(const D3DApp&) = delete;

	D3DApp() noexcept(false);

	~D3DApp();

	SIZE GetOutputSize() const {
		const auto outputSize = m_deviceResources->GetOutputSize();
		return { outputSize.right - outputSize.left, outputSize.bottom - outputSize.top };
	}

	void Tick();

	void OnWindowSizeChanged();

	void OnDisplayChange();

	void OnActivated();

	void OnDeactivated();

	void OnResuming();

	void OnSuspending();

	void OnDeviceLost() override;

	void OnDeviceRestored() override;

private:
	struct RenderItem {
		std::string Name;
		std::wstring HitGroup;
		struct { UINT Vertices = UINT_MAX, Indices = UINT_MAX; } DescriptorHeapIndices;
		struct { UINT Object = UINT_MAX; } ConstantBufferIndices;
		Material Material;
		TextureDictionary::mapped_type* pTextures{};
		physx::PxShape* Shape{};
	};

	std::unique_ptr<DX::DeviceResources> m_deviceResources = std::make_unique<decltype(m_deviceResources)::element_type>();

	std::unique_ptr<DirectX::GraphicsMemory> m_graphicsMemory;

	DX::StepTimer m_stepTimer;

	const std::unique_ptr<DirectX::GamePad> m_gamepad = std::make_unique<decltype(m_gamepad)::element_type>();
	DirectX::GamePad::ButtonStateTracker m_gamepadButtonStateTracker;

	const std::unique_ptr<DirectX::Keyboard> m_keyboard = std::make_unique<decltype(m_keyboard)::element_type>();
	DirectX::Keyboard::KeyboardStateTracker m_keyboardStateTracker;

	const std::unique_ptr<DirectX::Mouse> m_mouse = std::make_unique<decltype(m_mouse)::element_type>();
	DirectX::Mouse::ButtonStateTracker m_mouseButtonStateTracker;

	UINT m_raytracingSamplesPerPixel = 2;

	bool m_isTemporalAntiAliasingEnabled = true;
	decltype(DirectX::TemporalAntiAliasingEffect::Constants) m_temporalAntiAliasingConstants;

	TextureDictionary m_textures;

	std::vector<RenderItem> m_renderItems;

	std::unique_ptr<DirectX::DescriptorHeap> m_resourceDescriptors;

	Microsoft::WRL::ComPtr<ID3D12RootSignature> m_globalRootSignature;

	Microsoft::WRL::ComPtr<ID3D12StateObject> m_pipelineStateObject;

	std::unique_ptr<DirectX::TemporalAntiAliasingEffect> m_temporalAntiAliasingEffect;

	DirectX::GraphicsResource m_sceneConstantBuffer, m_objectConstantBuffer;

	std::unique_ptr<DirectX::RaytracingHelpers::Triangles<DirectX::VertexPositionNormalTexture, UINT16>> m_sphere;

	DirectX::RaytracingHelpers::AccelerationStructureBuffers
		m_sphereBottomLevelAccelerationStructureBuffers,
		m_topLevelAccelerationStructureBuffers;

	nv_helpers_dx12::ShaderBindingTableGenerator m_shaderBindingTableGenerator;
	Microsoft::WRL::ComPtr<ID3D12Resource> m_shaderBindingTable;

	Microsoft::WRL::ComPtr<ID3D12Resource> m_previousOutput, m_currentOutput, m_motionVectors, m_finalOutput;

	FirstPersonCamera m_firstPersonCamera;

	bool m_isMenuOpen{};

	bool m_isPhysicsSimulationRunning = true;
	MyPhysX m_myPhysX;

	struct Sphere {
		bool IsGravityEnabled;
		physx::PxVec3 Position;
		physx::PxReal Radius, RotationPeriod, OrbitalPeriod, Mass;
	} m_Moon{
		.Position = { -4, 4, 0 },
		.Radius = 0.25f,
		.OrbitalPeriod = 10
	}, m_Earth{
		.Position = { 0, m_Moon.Position.y, 0 },
		.Radius = 1,
		.RotationPeriod = 15,
		.Mass = PhysicsHelpers::UniversalGravitation::CalculateMass((m_Moon.Position - m_Earth.Position).magnitude(), m_Moon.OrbitalPeriod)
	}, m_star{
		.Position = { 0, -50.1f, 0 },
		.Radius = 50
	};

	struct { physx::PxReal PositionY = 0.5f, Period = 3; } m_spring;

	void CreateDeviceDependentResources();

	void CreateWindowSizeDependentResources();

	void Update();

	void Render();

	void Clear();

	void BuildTextures();

	void LoadTextures();

	void BuildRenderItems();

	void CreateDescriptorHeaps();

	void CreateRootSignatures();

	void CreatePipelineStateObjects();

	void CreateEffects();

	void CreateGeometries();

	void CreateBottomLevelAccelerationStructure(const std::vector<D3D12_RAYTRACING_GEOMETRY_DESC>& geometryDescs, DirectX::RaytracingHelpers::AccelerationStructureBuffers& buffers);

	void CreateTopLevelAccelerationStructure(bool updateOnly, DirectX::RaytracingHelpers::AccelerationStructureBuffers& buffers);

	void CreateAccelerationStructures();

	void CreateConstantBuffers();

	void CreateShaderBindingTables();

	void ProcessInput();

	void UpdateCamera(const DirectX::GamePad::State& gamepadStates, const DirectX::Keyboard::State& keyboardState, const DirectX::Mouse::State& mouseState);

	void UpdateSceneConstantBuffer();

	void UpdateRenderItem(RenderItem& renderItem) const;

	void DispatchRays();

	void DrawMenu();
};
