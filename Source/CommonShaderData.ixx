module;

#include "directxtk12/SimpleMath.h"

export module CommonShaderData;

import Material;

using namespace DirectX;

export {
	struct SceneResourceDescriptorHeapIndices {
		UINT InEnvironmentLightTexture = ~0u, InEnvironmentTexture = ~0u;
		XMUINT2 _;
	};

	struct SceneData {
		BOOL IsStatic, IsEnvironmentLightTextureCubeMap, IsEnvironmentTextureCubeMap;
		BOOL _;
		SceneResourceDescriptorHeapIndices ResourceDescriptorHeapIndices;
		XMFLOAT4 EnvironmentLightColor, EnvironmentColor;
		XMFLOAT3X4 EnvironmentLightTextureTransform, EnvironmentTextureTransform;
	};

	struct InstanceData {
		UINT FirstGeometryIndex;
		XMUINT3 _;
		XMFLOAT3X4 PreviousObjectToWorld, ObjectToWorld;
	};

	struct ObjectResourceDescriptorHeapIndices {
		struct { UINT Vertices = ~0u, Indices = ~0u, MotionVectors = ~0u, _; } Mesh;
		struct { UINT BaseColorMap = ~0u, EmissiveColorMap = ~0u, MetallicMap = ~0u, RoughnessMap = ~0u, AmbientOcclusionMap = ~0u, TransmissionMap = ~0u, OpacityMap = ~0u, NormalMap = ~0u; } Textures;
	};

	struct ObjectData {
		Material Material;
		ObjectResourceDescriptorHeapIndices ResourceDescriptorHeapIndices;
	};
}
