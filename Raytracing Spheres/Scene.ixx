module;

#include <d3d12.h>

#include "directxtk12/GamePad.h"
#include "directxtk12/Keyboard.h"
#include "directxtk12/Mouse.h"

#include "directxtk12/ResourceUploadBatch.h"

#include "directxtk12/VertexTypes.h"

#include "directxtk12/SimpleMath.h"

#include "PhysX.h"

#include <map>

#include <filesystem>

export module Scene;

import DirectX.DescriptorHeap;
import Material;
import Math;
import Model;
import ResourceHelpers;
import Texture;

using namespace DirectX;
using namespace DirectX::SimpleMath;
using namespace Math;
using namespace physx;
using namespace ResourceHelpers;
using namespace std;
using namespace std::filesystem;

export {
	struct RenderObjectBase {
		string Name;

		PxShape* Shape{};

		Material Material;

		Matrix TextureTransform;

		bool IsVisible = true;
	};

	struct RenderObjectDesc : RenderObjectBase {
		string MeshURI;

		map<TextureType, path> Textures;
	};

	struct RenderObject : RenderObjectBase {
		shared_ptr<Mesh> Mesh;

		map<TextureType, Texture> Textures;
	};

	struct SceneBase {
		virtual ~SceneBase() = default;

		struct {
			Vector3 Position;
			Quaternion Rotation;
		} Camera;

		Color EnvironmentLightColor{ 0, 0, 0, -1 }, EnvironmentColor{ 0, 0, 0, -1 };

		shared_ptr<PhysX> PhysX;

		unordered_map<string, PxRigidBody*> RigidBodies;
	};

	struct SceneDesc : SceneBase {
		struct {
			path FilePath;
			Transform Transform;
		} EnvironmentLightCubeMap, EnvironmentCubeMap;

		unordered_map<string, pair<vector<VertexPositionNormalTexture>, vector<UINT16>>> Meshes;

		vector<RenderObjectDesc> RenderObjects;
	};

	struct Scene : SceneBase {
		struct {
			Texture Texture;
			Transform Transform;
		} EnvironmentLightCubeMap, EnvironmentCubeMap;

		unordered_map<string, shared_ptr<Mesh>> Meshes;

		vector<RenderObject> RenderObjects;

		virtual bool IsWorldStatic() const = 0;

		virtual void Tick(double elapsedSeconds, const GamePad::ButtonStateTracker& gamepadStateTracker, const Keyboard::KeyboardStateTracker& keyboardStateTracker, const Mouse::ButtonStateTracker& mouseStateTracker) = 0;

		void Load(const SceneDesc& sceneDesc, ID3D12Device* pDevice, ID3D12CommandQueue* pCommandQueue, DescriptorHeapEx& descriptorHeap, _Inout_ UINT& descriptorHeapIndex) {
			reinterpret_cast<SceneBase&>(*this) = sceneDesc;

			ResourceUploadBatch resourceUploadBatch(pDevice);
			resourceUploadBatch.Begin();

			{
				if (!sceneDesc.EnvironmentLightCubeMap.FilePath.empty()) {
					EnvironmentLightCubeMap.Texture.Load(ResolveResourcePath(sceneDesc.EnvironmentLightCubeMap.FilePath), pDevice, resourceUploadBatch, descriptorHeap, descriptorHeapIndex);
					EnvironmentLightCubeMap.Transform = sceneDesc.EnvironmentLightCubeMap.Transform;
				}

				if (!sceneDesc.EnvironmentCubeMap.FilePath.empty()) {
					EnvironmentCubeMap.Texture.Load(ResolveResourcePath(sceneDesc.EnvironmentCubeMap.FilePath), pDevice, resourceUploadBatch, descriptorHeap, descriptorHeapIndex);
					EnvironmentCubeMap.Transform = sceneDesc.EnvironmentCubeMap.Transform;
				}
			}

			{
				for (const auto& [URI, Mesh] : sceneDesc.Meshes) {
					Meshes[URI] = Mesh::Create(Mesh.first, Mesh.second, pDevice, resourceUploadBatch, descriptorHeap, descriptorHeapIndex);
				}

				for (const auto& renderObjectDesc : sceneDesc.RenderObjects) {
					RenderObject renderObject;
					reinterpret_cast<RenderObjectBase&>(renderObject) = renderObjectDesc;

					renderObject.Mesh = Meshes.at(renderObjectDesc.MeshURI);

					for (const auto& [TextureType, FilePath] : renderObjectDesc.Textures) {
						Texture texture;
						texture.Load(ResolveResourcePath(FilePath), pDevice, resourceUploadBatch, descriptorHeap, descriptorHeapIndex);
						renderObject.Textures[TextureType] = texture;
					}

					RenderObjects.emplace_back(renderObject);
				}
			}

			resourceUploadBatch.End(pCommandQueue).get();
		}
	};
}
