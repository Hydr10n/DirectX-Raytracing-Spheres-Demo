#pragma once

#pragma warning(push)

#pragma warning(disable: 26812)

#include "pch.h"

#include "DeviceResources.h"

#include "StepTimer.h"

#include "GraphicsMemory.h"

#include "GamePad.h"
#include "Keyboard.h"
#include "Mouse.h"

#include "DescriptorHeap.h"

#include "VertexTypes.h"

#include "ShaderBindingTableGenerator.h"

#include "RaytracingGeometryHelpers.h"

#include "Material.h"
#include "Texture.h"

#include "Camera.h"

#include "MyPhysX.h"

#include <map>

#pragma warning(pop)

class D3DApp : public DX::IDeviceNotify {
public:
	static constexpr UINT MaxAntiAliasingSampleCount = 8;

	D3DApp(const D3DApp&) = delete;
	D3DApp& operator=(const D3DApp&) = delete;

	D3DApp(HWND hWnd, const SIZE& outputSize) noexcept(false);

	~D3DApp();

	UINT GetAntiAliasingSampleCount() const { return m_antiAliasingSampleCount; }

	bool SetAntiAliasingSampleCount(UINT count) {
		if (count < 1 || count > MaxAntiAliasingSampleCount) return false;
		m_antiAliasingSampleCount = count;
		return true;
	}

	SIZE GetOutputSize() const {
		const auto rc = m_deviceResources->GetOutputSize();
		return { rc.right - rc.left, rc.bottom - rc.top };
	}

	void Tick();

	void OnWindowSizeChanged(const SIZE& size);

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
		size_t VerticesDescriptorHeapIndex = SIZE_MAX, IndicesDescriptorHeapIndex = SIZE_MAX, ObjectConstantBufferIndex = SIZE_MAX;
		Material Material;
		Texture* pImageTexture{};
		physx::PxShape* Shape{};
	};

	const std::unique_ptr<DirectX::GamePad> m_gamepad = std::make_unique<decltype(m_gamepad)::element_type>();
	const std::unique_ptr<DirectX::Keyboard> m_keyboard = std::make_unique<decltype(m_keyboard)::element_type>();
	const std::unique_ptr<DirectX::Mouse> m_mouse = std::make_unique<decltype(m_mouse)::element_type>();

	DirectX::GamePad::ButtonStateTracker m_gamepadButtonStateTrackers[DirectX::GamePad::MAX_PLAYER_COUNT];
	DirectX::Keyboard::KeyboardStateTracker m_keyboardStateTracker;
	DirectX::Mouse::ButtonStateTracker m_mouseButtonStateTracker;

	std::unique_ptr<DX::DeviceResources> m_deviceResources = std::make_unique<decltype(m_deviceResources)::element_type>();
	std::unique_ptr<DirectX::GraphicsMemory> m_graphicsMemory;

	DX::StepTimer m_stepTimer;

	UINT m_antiAliasingSampleCount{};

	std::map<std::string, std::shared_ptr<Texture>> m_imageTextures;

	std::vector<RenderItem> m_renderItems;

	Microsoft::WRL::ComPtr<ID3D12RootSignature> m_globalRootSignature;

	Microsoft::WRL::ComPtr<ID3D12StateObject> m_pipelineStateObject;

	std::unique_ptr<DirectX::DescriptorHeap> m_resourceDescriptors;

	DirectX::GraphicsResource m_sceneConstantBuffer, m_objectConstantBuffer;

	static constexpr float SphereRadius = 0.5f;
	std::shared_ptr<RaytracingHelpers::Triangles<DirectX::VertexPositionNormalTexture, UINT16>> m_sphere;

	RaytracingHelpers::AccelerationStructureBuffers m_sphereBottomLevelAccelerationStructureBuffers, m_topLevelAccelerationStructureBuffers;

	nv_helpers_dx12::ShaderBindingTableGenerator m_shaderBindingTableGenerator;
	Microsoft::WRL::ComPtr<ID3D12Resource> m_shaderBindingTable;

	Microsoft::WRL::ComPtr<ID3D12Resource> m_output;

	Camera m_camera = decltype(m_camera)(false);

	MyPhysX m_myPhysX;

	struct Sphere {
		bool IsGravityEnabled;
		physx::PxVec3 Position;
		physx::PxReal Radius, RotationPeriod, OrbitalPeriod, Mass;
	} m_Moon{
		.Position = { 0, 4, 4 },
		.Radius = 0.25f,
		.OrbitalPeriod = 10
	}, m_Earth{
		.Position = { 0, m_Moon.Position.y, 0 },
		.Radius = 1,
		.RotationPeriod = 15,
		.Mass = PhysicsHelpers::Gravity::CalculateMass((m_Moon.Position - m_Earth.Position).magnitude(), m_Moon.OrbitalPeriod)
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

	void CreateRootSignatures();

	void CreatePipelineStateObjects();

	void CreateDescriptorHeaps();

	void CreateGeometries();

	void BuildBottomLevelAccelerationStructure(const std::vector<D3D12_RAYTRACING_GEOMETRY_DESC>& geometryDescs, RaytracingHelpers::AccelerationStructureBuffers& buffers);

	void BuildTopLevelAccelerationStructure(bool updateOnly, RaytracingHelpers::AccelerationStructureBuffers& buffers);

	void CreateAccelerationStructures();

	void CreateConstantBuffers();

	void CreateShaderBindingTables();

	void CreateDeviceDependentShaderResourceViews();

	void CreateWindowSizeDependentShaderResourceViews();

	void ProcessInput();

	void UpdateCamera(const DirectX::GamePad::State(&gamepadStates)[DirectX::GamePad::MAX_PLAYER_COUNT], const DirectX::Keyboard::State& keyboardState, const DirectX::Mouse::State& mouseState);

	void ToggleGravities();

	void UpdateSceneConstantBuffer();

	void UpdateRenderItem(RenderItem& renderItem) const;

	void DispatchRays();
};
