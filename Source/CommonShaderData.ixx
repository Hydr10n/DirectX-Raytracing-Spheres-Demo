module;

#include <d3d12.h>

#include "directxtk12/SimpleMath.h"

export module CommonShaderData;

import Material;
import Vertex;

using namespace DirectX;

export {
	struct SceneResourceDescriptorIndices {
		UINT EnvironmentLightTexture = ~0u, EnvironmentTexture = ~0u;
		XMUINT2 _;
	};

	struct alignas(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT) SceneData {
		BOOL IsStatic, IsEnvironmentLightTextureCubeMap, IsEnvironmentTextureCubeMap;
		BOOL _;
		XMFLOAT4 EnvironmentLightColor, EnvironmentColor;
		XMFLOAT3X4 EnvironmentLightTextureTransform, EnvironmentTextureTransform;
		SceneResourceDescriptorIndices ResourceDescriptorIndices;
	};

	struct InstanceData {
		UINT FirstGeometryIndex;
		XMUINT3 _;
		XMFLOAT3X4 PreviousObjectToWorld, ObjectToWorld;
	};

	struct MeshResourceDescriptorIndices { UINT Vertices = ~0u, Indices = ~0u, MotionVectors = ~0u, _; };

	struct TextureMapResourceDescriptorIndices {
		UINT
			BaseColor = ~0u,
			EmissiveColor = ~0u,
			Metallic = ~0u,
			Roughness = ~0u,
			AmbientOcclusion = ~0u,
			Transmission = ~0u,
			Opacity = ~0u,
			Normal = ~0u;
	};

	struct ObjectResourceDescriptorIndices {
		MeshResourceDescriptorIndices Mesh;
		TextureMapResourceDescriptorIndices Textures;
	};

	struct ObjectData {
		VertexDesc VertexDesc;
		Material Material;
		ObjectResourceDescriptorIndices ResourceDescriptorIndices;
	};
}
