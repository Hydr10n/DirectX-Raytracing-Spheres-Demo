module;

#include <filesystem>
#include <map>

#include "directxtk12/GamePad.h"
#include "directxtk12/Keyboard.h"
#include "directxtk12/Mouse.h"
#include "directxtk12/ResourceUploadBatch.h"
#include "directxtk12/SimpleMath.h"
#include "directxtk12/VertexTypes.h"

#include "rtxmu/D3D12AccelStructManager.h"

#include "PhysX.h"

export module Scene;

import CommandList;
import DescriptorHeap;
import Event;
import Math;
import Material;
import Model;
import RaytracingHelpers;
import ResourceHelpers;
import TextureHelpers;

using namespace DirectX;
using namespace DirectX::RaytracingHelpers;
using namespace DirectX::SimpleMath;
using namespace Math;
using namespace physx;
using namespace ResourceHelpers;
using namespace rtxmu;
using namespace std;
using namespace std::filesystem;
using namespace TextureHelpers;

export {
	struct RenderObjectBase {
		string Name;

		PxShape* Shape{};

		Material Material;

		bool IsVisible = true;
	};

	struct RenderObjectDesc : RenderObjectBase {
		string MeshURI;

		map<TextureMap, path> Textures;
	};

	struct RenderObject : RenderObjectBase {
		shared_ptr<Mesh> Mesh;

		map<TextureMap, shared_ptr<Texture>> Textures;
	};

	struct SceneBase {
		virtual ~SceneBase() = default;

		struct {
			Vector3 Position;
			Quaternion Rotation;
		} Camera;

		Color EnvironmentLightColor{ 0, 0, 0, -1 }, EnvironmentColor{ 0, 0, 0, -1 };

		shared_ptr<PhysX> PhysX;

		unordered_map<string, PxRigidActor*> RigidActors;
	};

	struct SceneDesc : SceneBase {
		struct {
			path FilePath;
			Transform Transform;
		} EnvironmentLightTexture, EnvironmentTexture;

		unordered_map<string, pair<shared_ptr<vector<DirectX::VertexPositionNormalTexture>>, shared_ptr<vector<Mesh::IndexType>>>> Meshes;

		vector<RenderObjectDesc> RenderObjects;
	};

	struct Scene : SceneBase {
		struct InstanceData {
			UINT FirstGeometryIndex;
			XMFLOAT3X4 PreviousObjectToWorld, ObjectToWorld;
		};

		struct {
			shared_ptr<Texture> Texture;
			Transform Transform;
		} EnvironmentLightTexture, EnvironmentTexture;

		unordered_map<string, shared_ptr<Mesh>> Meshes;

		vector<RenderObject> RenderObjects;

		Scene(ID3D12Device5* pDevice, ID3D12CommandQueue* pCommandQueue) : m_device(pDevice), m_commandQueue(pCommandQueue), m_commandList(pDevice), m_observer(*this) {}

		virtual bool IsStatic() const { return false; }

		virtual void Tick(double elapsedSeconds, const GamePad::ButtonStateTracker& gamepadStateTracker, const Keyboard::KeyboardStateTracker& keyboardStateTracker, const Mouse::ButtonStateTracker& mouseStateTracker) {}

		void Load(const SceneDesc& sceneDesc, DescriptorHeapEx& descriptorHeap, _Inout_ UINT& descriptorIndex) {
			reinterpret_cast<SceneBase&>(*this) = sceneDesc;

			ResourceUploadBatch resourceUploadBatch(m_device);
			resourceUploadBatch.Begin();

			{
				if (!empty(sceneDesc.EnvironmentLightTexture.FilePath)) {
					EnvironmentLightTexture.Texture = LoadTexture(ResolveResourcePath(sceneDesc.EnvironmentLightTexture.FilePath), m_device, resourceUploadBatch, descriptorHeap, descriptorIndex);
					EnvironmentLightTexture.Transform = sceneDesc.EnvironmentLightTexture.Transform;
				}
				if (!empty(sceneDesc.EnvironmentTexture.FilePath)) {
					EnvironmentTexture.Texture = LoadTexture(ResolveResourcePath(sceneDesc.EnvironmentTexture.FilePath), m_device, resourceUploadBatch, descriptorHeap, descriptorIndex);
					EnvironmentTexture.Transform = sceneDesc.EnvironmentTexture.Transform;
				}
			}

			{
				for (const auto& [URI, Mesh] : sceneDesc.Meshes) {
					Meshes[URI] = Mesh::Create(*Mesh.first, *Mesh.second, m_device, resourceUploadBatch, descriptorHeap, descriptorIndex);
				}

				for (const auto& renderObjectDesc : sceneDesc.RenderObjects) {
					RenderObject renderObject;
					reinterpret_cast<RenderObjectBase&>(renderObject) = renderObjectDesc;

					renderObject.Mesh = Meshes.at(renderObjectDesc.MeshURI);

					for (const auto& [TextureType, FilePath] : renderObjectDesc.Textures) {
						renderObject.Textures[TextureType] = LoadTexture(ResolveResourcePath(FilePath), m_device, resourceUploadBatch, descriptorHeap, descriptorIndex);
					}

					RenderObjects.emplace_back(renderObject);
				}
			}

			resourceUploadBatch.End(m_commandQueue).get();

			Refresh();

			CreateAccelerationStructures(false);
		}

		const auto& GetInstanceData() const noexcept { return m_instanceData; }

		auto GetObjectCount() const noexcept { return m_objectCount; }

		void Refresh() {
			UINT instanceIndex = 0, objectIndex = 0;
			for (const auto& renderObject : RenderObjects) {
				const auto Transform = [&] {
					const auto& shape = *renderObject.Shape;

					PxVec3 scaling;
					switch (const PxGeometryHolder geometry = shape.getGeometry(); geometry.getType()) {
						case PxGeometryType::eSPHERE: scaling = PxVec3(2 * geometry.sphere().radius); break;
						default: throw;
					}

					PxMat44 world(PxVec4(1, 1, -1, 1));
					world *= PxShapeExt::getGlobalPose(shape, *shape.getActor());
					world.scale(PxVec4(scaling, 1));
					XMFLOAT3X4 transform;
					XMStoreFloat3x4(&transform, reinterpret_cast<const XMMATRIX&>(*world.front()));
					return transform;
				};
				InstanceData instanceData;
				instanceData.FirstGeometryIndex = objectIndex;
				if (instanceIndex >= size(m_instanceData)) {
					instanceData.PreviousObjectToWorld = instanceData.ObjectToWorld = Transform();
					m_instanceData.emplace_back(instanceData);
				}
				else {
					instanceData.PreviousObjectToWorld = m_instanceData[instanceIndex].ObjectToWorld;
					instanceData.ObjectToWorld = Transform();
					m_instanceData[instanceIndex] = instanceData;
				}
				instanceIndex++;
				objectIndex++;
			}
			m_objectCount = objectIndex;
		}

		const auto& GetTopLevelAccelerationStructure() const { return *m_topLevelAccelerationStructure; }

		void CreateAccelerationStructures(bool updateOnly) {
			vector<uint64_t> newBottomLevelAccelerationStructureIDs;

			{
				m_commandList.Begin();

				if (!updateOnly) {
					m_bottomLevelAccelerationStructureIDs = {};

					m_accelerationStructureManager = make_unique<DxAccelStructManager>(m_device);
					m_accelerationStructureManager->Initialize();
				}

				vector<vector<D3D12_RAYTRACING_GEOMETRY_DESC>> geometryDescs;
				vector<D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS> newBuildBottomLevelAccelerationStructureInputs;
				vector<const Mesh*> newMeshes;

				for (const auto& renderObject : RenderObjects) {
					const auto mesh = renderObject.Mesh.get();
					if (const auto [first, second] = m_bottomLevelAccelerationStructureIDs.try_emplace(mesh); second) {
						auto& _geometryDescs = geometryDescs.emplace_back(initializer_list{ CreateGeometryDesc(*mesh->Vertices, *mesh->Indices, renderObject.Material.AlphaMode == AlphaMode::Opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE) });
						newBuildBottomLevelAccelerationStructureInputs.emplace_back(D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS{
							.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL,
							.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION,
							.NumDescs = static_cast<UINT>(size(_geometryDescs)),
							.pGeometryDescs = data(_geometryDescs)
							});
						newMeshes.emplace_back(mesh);
					}
				}

				m_objectCount = static_cast<UINT>(size(RenderObjects));

				if (!empty(newBuildBottomLevelAccelerationStructureInputs)) {
					m_accelerationStructureManager->PopulateBuildCommandList(m_commandList, data(newBuildBottomLevelAccelerationStructureInputs), size(newBuildBottomLevelAccelerationStructureInputs), newBottomLevelAccelerationStructureIDs);
					for (UINT i = 0; const auto & meshNode : newMeshes) m_bottomLevelAccelerationStructureIDs[meshNode] = newBottomLevelAccelerationStructureIDs[i++];
					m_accelerationStructureManager->PopulateUAVBarriersCommandList(m_commandList, newBottomLevelAccelerationStructureIDs);
					m_accelerationStructureManager->PopulateCompactionSizeCopiesCommandList(m_commandList, newBottomLevelAccelerationStructureIDs);
				}

				m_commandList.End(m_commandQueue).get();
			}

			{
				m_commandList.Begin();

				if (!empty(newBottomLevelAccelerationStructureIDs)) m_accelerationStructureManager->PopulateCompactionCommandList(m_commandList, newBottomLevelAccelerationStructureIDs);

				if (!updateOnly) {
					m_topLevelAccelerationStructure = make_unique<TopLevelAccelerationStructure>(m_device, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE);
				}
				vector<D3D12_RAYTRACING_INSTANCE_DESC> instanceDescs;
				instanceDescs.reserve(size(m_instanceData));
				for (UINT instanceIndex = 0; const auto & renderObject : RenderObjects) {
					auto& instanceDesc = instanceDescs.emplace_back(D3D12_RAYTRACING_INSTANCE_DESC{
						.InstanceMask = renderObject.IsVisible ? ~0u : 0,
						.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE | D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_FRONT_COUNTERCLOCKWISE,
						.AccelerationStructure = m_accelerationStructureManager->GetAccelStructGPUVA(m_bottomLevelAccelerationStructureIDs.at(renderObject.Mesh.get()))
						});
					reinterpret_cast<XMFLOAT3X4&>(instanceDesc.Transform) = m_instanceData[instanceIndex++].ObjectToWorld;
				}
				m_topLevelAccelerationStructure->Build(m_commandList, instanceDescs, updateOnly);

				m_commandList.End(m_commandQueue).get();
			}

			if (!empty(newBottomLevelAccelerationStructureIDs)) m_accelerationStructureManager->GarbageCollection(newBottomLevelAccelerationStructureIDs);
		}

	private:
		ID3D12Device5* m_device;
		ID3D12CommandQueue* m_commandQueue;
		CommandList<ID3D12GraphicsCommandList4> m_commandList;

		vector<InstanceData> m_instanceData;
		UINT m_objectCount{};

		unique_ptr<DxAccelStructManager> m_accelerationStructureManager;
		unordered_map<const Mesh*, uint64_t> m_bottomLevelAccelerationStructureIDs;
		unique_ptr<TopLevelAccelerationStructure> m_topLevelAccelerationStructure;

		struct Observer {
			Observer(Scene& scene) : m_scene(scene) {
				m_meshDelete = Mesh::DeleteEvent += [&](const Mesh* ptr) {
					scene.m_bottomLevelAccelerationStructureIDs.erase(ptr);
				};
			}

			~Observer() { Mesh::DeleteEvent -= m_meshDelete; }

		private:
			Scene& m_scene;
			EventHandle m_meshDelete{};
		} m_observer;
	};
}
