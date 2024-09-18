module;

#include <filesystem>

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
import DeviceContext;
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

		path Textures[to_underlying(TextureMapType::_2DCount)];
	};

	struct RenderObject : RenderObjectBase {
		shared_ptr<Mesh> Mesh;

		shared_ptr<Texture> Textures[to_underlying(TextureMapType::_2DCount)];
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
			AffineTransform Transform;
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
			AffineTransform Transform;
		} EnvironmentLightTexture, EnvironmentTexture;

		unordered_map<string, shared_ptr<Mesh>> Meshes;

		vector<RenderObject> RenderObjects;

		explicit Scene(const DeviceContext& deviceContext) : m_deviceContext(deviceContext) {}

		~Scene() override {
			vector<uint64_t> IDs;
			IDs.reserve(size(m_bottomLevelAccelerationStructureIDs) + 1);
			for (const auto& [MeshNode, ID] : m_bottomLevelAccelerationStructureIDs) {
				IDs.emplace_back(ID.first);
				MeshNode->OnDestroyed.remove(ID.second);
			}
			IDs.emplace_back(m_topLevelAccelerationStructure.ID);
			m_deviceContext.AccelerationStructureManager->RemoveAccelerationStructures(IDs);
			m_bottomLevelAccelerationStructureIDs = {};
			m_topLevelAccelerationStructure = {};

			CollectGarbage();
		}

		virtual bool IsStatic() const { return false; }

		virtual void Tick(double elapsedSeconds, const GamePad::ButtonStateTracker& gamepadStateTracker, const Keyboard::KeyboardStateTracker& keyboardStateTracker, const Mouse::ButtonStateTracker& mouseStateTracker) = 0;

		void Load(const SceneDesc& sceneDesc) {
			reinterpret_cast<SceneBase&>(*this) = sceneDesc;

			CommandList commandList(m_deviceContext);
			commandList.Begin();

			ResourceUploadBatch resourceUploadBatch(m_deviceContext.Device);
			resourceUploadBatch.Begin();

			{
				if (!empty(sceneDesc.EnvironmentLightTexture.FilePath)) {
					EnvironmentLightTexture.Texture = LoadTexture(ResolveResourcePath(sceneDesc.EnvironmentLightTexture.FilePath), m_deviceContext, resourceUploadBatch);
					EnvironmentLightTexture.Transform = sceneDesc.EnvironmentLightTexture.Transform;
				}
				if (!empty(sceneDesc.EnvironmentTexture.FilePath)) {
					EnvironmentTexture.Texture = LoadTexture(ResolveResourcePath(sceneDesc.EnvironmentTexture.FilePath), m_deviceContext, resourceUploadBatch);
					EnvironmentTexture.Transform = sceneDesc.EnvironmentTexture.Transform;
				}
			}

			{
				for (const auto& [URI, Mesh] : sceneDesc.Meshes) {
					Meshes[URI] = Mesh::Create(*Mesh.first, *Mesh.second, m_deviceContext, commandList);
				}

				for (const auto& renderObjectDesc : sceneDesc.RenderObjects) {
					RenderObject renderObject;
					reinterpret_cast<RenderObjectBase&>(renderObject) = renderObjectDesc;

					renderObject.Mesh = Meshes.at(renderObjectDesc.MeshURI);

					for (size_t i = 0; const auto & filePath : renderObjectDesc.Textures) {
						if (!empty(filePath)) {
							renderObject.Textures[i] = LoadTexture(ResolveResourcePath(filePath), m_deviceContext, resourceUploadBatch);
						}
						i++;
					}

					RenderObjects.emplace_back(renderObject);
				}
			}

			resourceUploadBatch.End(m_deviceContext.CommandQueue).get();

			Tick(0);

			Refresh();

			CreateAccelerationStructures(commandList);

			commandList.End();

			commandList.Begin();

			commandList.CompactAccelerationStructures();

			commandList.End();
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
				if (instanceIndex < size(m_instanceData)) {
					instanceData.PreviousObjectToWorld = m_instanceData[instanceIndex].ObjectToWorld;
					instanceData.ObjectToWorld = Transform();
					m_instanceData[instanceIndex] = instanceData;
				}
				else {
					instanceData.PreviousObjectToWorld = instanceData.ObjectToWorld = Transform();
					m_instanceData.emplace_back(instanceData);
				}
				instanceIndex++;
				objectIndex++;
			}
			m_objectCount = objectIndex;
		}

		auto GetTopLevelAccelerationStructure() const {
			return m_deviceContext.AccelerationStructureManager->GetAccelStructGPUVA(m_topLevelAccelerationStructure.ID);
		}

		void CreateAccelerationStructures(CommandList& commandList) {
			auto& accelerationStructureManager = *m_deviceContext.AccelerationStructureManager;

			{
				vector<vector<D3D12_RAYTRACING_GEOMETRY_DESC>> geometryDescs;
				vector<D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS> newInputs;
				vector<Mesh*> newMeshes;

				for (const auto& renderObject : RenderObjects) {
					const auto mesh = renderObject.Mesh.get();
					const auto [first, second] = m_bottomLevelAccelerationStructureIDs.try_emplace(mesh);
					if (!second) continue;

					auto& _geometryDescs = geometryDescs.emplace_back(initializer_list{ CreateGeometryDesc(*mesh->Vertices, *mesh->Indices, renderObject.Material.AlphaMode == AlphaMode::Opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE) });
					newInputs.emplace_back(D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS{
						.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL,
						.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION,
						.NumDescs = static_cast<UINT>(size(_geometryDescs)),
						.pGeometryDescs = data(_geometryDescs)
						});
					newMeshes.emplace_back(mesh);
				}

				if (!empty(newInputs)) {
					const auto IDs = commandList.BuildAccelerationStructures(newInputs);

					for (size_t i = 0; const auto & mesh : newMeshes) {
						auto& ID = m_bottomLevelAccelerationStructureIDs.at(mesh);
						ID.first = IDs[i++];
						ID.second = mesh->OnDestroyed.append([&](Mesh* pMesh) {
							if (const auto pID = m_bottomLevelAccelerationStructureIDs.find(pMesh);
							pID != cend(m_bottomLevelAccelerationStructureIDs)) {
							m_unreferencedBottomLevelAccelerationStructureIDs.emplace_back(pID->second.first);
							m_bottomLevelAccelerationStructureIDs.erase(pID);
						}
							});
					}
				}
			}

			vector<D3D12_RAYTRACING_INSTANCE_DESC> instanceDescs;
			instanceDescs.reserve(size(m_instanceData));
			for (UINT instanceIndex = 0; const auto & renderObject : RenderObjects) {
				const auto& instanceData = m_instanceData[instanceIndex++];
				auto& instanceDesc = instanceDescs.emplace_back(D3D12_RAYTRACING_INSTANCE_DESC{
					.InstanceID = instanceData.FirstGeometryIndex,
					.InstanceMask = renderObject.IsVisible ? ~0u : 0,
					.InstanceContributionToHitGroupIndex = instanceData.FirstGeometryIndex,
					.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE | D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_FRONT_COUNTERCLOCKWISE,
					.AccelerationStructure = accelerationStructureManager.GetAccelStructGPUVA(m_bottomLevelAccelerationStructureIDs.at(renderObject.Mesh.get()).first)
					});
				reinterpret_cast<XMFLOAT3X4&>(instanceDesc.Transform) = instanceData.ObjectToWorld;
			}
			BuildTopLevelAccelerationStructure(commandList, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE, instanceDescs, false, m_topLevelAccelerationStructure);
		}

		void CollectGarbage() {
			if (!empty(m_unreferencedBottomLevelAccelerationStructureIDs)) {
				m_deviceContext.AccelerationStructureManager->RemoveAccelerationStructures(m_unreferencedBottomLevelAccelerationStructureIDs);
				m_unreferencedBottomLevelAccelerationStructureIDs.clear();
			}
		}

	protected:
		virtual void Tick(double elapsedSeconds) = 0;

	private:
		const DeviceContext& m_deviceContext;

		vector<InstanceData> m_instanceData;
		UINT m_objectCount{};

		vector<uint64_t> m_unreferencedBottomLevelAccelerationStructureIDs;
		unordered_map<Mesh*, pair<uint64_t, Mesh::DestroyEvent::Handle>> m_bottomLevelAccelerationStructureIDs;
		TopLevelAccelerationStructure m_topLevelAccelerationStructure;
	};
}
