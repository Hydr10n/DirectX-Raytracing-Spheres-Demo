module;

#include "directxtk12/SimpleMath.h"

export module CommonShaderData;

import Material;

using namespace DirectX;

export {
	struct SceneData {
		BOOL IsStatic, IsEnvironmentLightTextureCubeMap, IsEnvironmentTextureCubeMap;
		BOOL _;
		XMFLOAT4 EnvironmentLightColor;
		XMFLOAT4X4 EnvironmentLightTextureTransform;
		XMFLOAT4 EnvironmentColor;
		XMFLOAT4X4 EnvironmentTextureTransform;
	};

	struct InstanceData {
		BOOL IsStatic;
		XMFLOAT4X4 PreviousObjectToWorld;
	};

	struct ObjectResourceDescriptorHeapIndices {
		struct { UINT Vertices = ~0u, Indices = ~0u, MotionVectors = ~0u; } Mesh;
		struct { UINT BaseColorMap = ~0u, EmissiveColorMap = ~0u, MetallicMap = ~0u, RoughnessMap = ~0u, AmbientOcclusionMap = ~0u, TransmissionMap = ~0u, OpacityMap = ~0u, NormalMap = ~0u; } Textures;
	};

	struct ObjectData {
		Material Material;
		XMFLOAT4X4 TextureTransform;
	};
}
