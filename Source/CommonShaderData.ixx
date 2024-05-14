module;

#include "directxtk12/SimpleMath.h"

export module CommonShaderData;

import Material;
import NRD;
import Vertex;

using namespace DirectX;

export {
	struct SceneResourceDescriptorIndices {
		UINT InEnvironmentLightTexture = ~0u, InEnvironmentTexture = ~0u;
		XMUINT2 _;
	};

	struct SceneData {
		BOOL IsStatic, IsEnvironmentLightTextureCubeMap, IsEnvironmentTextureCubeMap;
		BOOL _;
		SceneResourceDescriptorIndices ResourceDescriptorIndices;
		XMFLOAT4 EnvironmentLightColor, EnvironmentColor;
		XMFLOAT3X4 EnvironmentLightTextureTransform, EnvironmentTextureTransform;
	};

	struct InstanceData {
		UINT FirstGeometryIndex;
		XMUINT3 _;
		XMFLOAT3X4 PreviousObjectToWorld, ObjectToWorld;
	};

	struct ObjectResourceDescriptorIndices {
		struct { UINT Vertices = ~0u, Indices = ~0u, MotionVectors = ~0u, _; } Mesh;
		struct { UINT BaseColorMap = ~0u, EmissiveColorMap = ~0u, MetallicMap = ~0u, RoughnessMap = ~0u, AmbientOcclusionMap = ~0u, TransmissionMap = ~0u, OpacityMap = ~0u, NormalMap = ~0u; } Textures;
	};

	struct ObjectData {
		VertexDesc VertexDesc;
		Material Material;
		ObjectResourceDescriptorIndices ResourceDescriptorIndices;
	};

	struct NRDSettings {
		NRDDenoiser Denoiser;
		XMUINT3 _;
		XMFLOAT4 HitDistanceParameters;
	};
}
