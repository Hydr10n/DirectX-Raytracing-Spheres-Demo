module;

#include <d3d12.h>

#include "directxtk12/SimpleMath.h"

export module CommonShaderData;

import Material;
import Vertex;

using namespace DirectX;

export {
	struct alignas(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT) SceneData {
		uint32_t IsStatic, IsEnvironmentLightTextureCubeMap;
		uint32_t EnvironmentLightTextureDescriptor = ~0u, _;
		XMFLOAT4 EnvironmentLightColor;
		XMFLOAT3X4 EnvironmentLightTransform;
	};

	struct InstanceData {
		uint32_t FirstGeometryIndex;
		XMUINT3 _;
		XMFLOAT3X4 PreviousObjectToWorld, ObjectToWorld;
	};

	struct MeshDescriptors {
		uint32_t Vertices = ~0u, Indices = ~0u, MotionVectors = ~0u, _;
	};

	using TextureMapInfoArray = TextureMapInfo[TextureMapType::Count];

	struct ObjectData {
		VertexDesc VertexDesc;
		MeshDescriptors MeshDescriptors;
		Material Material;
		TextureMapInfoArray TextureMapInfoArray;
	};
}
