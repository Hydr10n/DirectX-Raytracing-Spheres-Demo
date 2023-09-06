module;

#include "directxtk12/SimpleMath.h"

export module RaytracingShaderData;

import Material;

using namespace DirectX;

export {
	struct GlobalResourceDescriptorHeapIndices {
		UINT
			InGraphicsSettings = ~0u,
			InCamera = ~0u,
			InSceneData = ~0u,
			InInstanceData = ~0u,
			InObjectResourceDescriptorHeapIndices = ~0u, InObjectData = ~0u,
			InEnvironmentLightCubeMap = ~0u, InEnvironmentCubeMap = ~0u,
			Output = ~0u,
			OutDepth = ~0u,
			OutMotionVectors = ~0u,
			OutBaseColorMetalness = ~0u,
			OutEmissiveColor = ~0u,
			OutNormalRoughness = ~0u,
			OutNoisyDiffuse = ~0u, OutNoisySpecular = ~0u;
	};

	struct GraphicsSettings {
		UINT FrameIndex, MaxTraceRecursionDepth, SamplesPerPixel;
		BOOL IsRussianRouletteEnabled;
		XMFLOAT4 NRDHitDistanceParameters;
	};

	struct SceneData {
		BOOL IsStatic;
		XMUINT3 _;
		XMFLOAT4 EnvironmentLightColor;
		XMFLOAT4X4 EnvironmentLightCubeMapTransform;
		XMFLOAT4 EnvironmentColor;
		XMFLOAT4X4 EnvironmentCubeMapTransform;
	};

	struct InstanceData {
		BOOL IsStatic;
		XMFLOAT4X4 PreviousObjectToWorld;
	};

	struct ObjectResourceDescriptorHeapIndices {
		struct { UINT Vertices = ~0u, Indices = ~0u, MotionVectors = ~0u; } Mesh;
		struct {
			UINT BaseColorMap = ~0u, EmissiveColorMap = ~0u, MetallicMap = ~0u, RoughnessMap = ~0u, AmbientOcclusionMap = ~0u, TransmissionMap = ~0u, OpacityMap = ~0u, NormalMap = ~0u;
		} Textures;
	};

	struct ObjectData {
		Material Material;
		XMFLOAT4X4 TextureTransform;
	};
}
