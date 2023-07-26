module;

#include "directxtk12/SimpleMath.h"

export module RaytracingShaderData;

import Material;

using namespace DirectX;

export {
	struct GlobalResourceDescriptorHeapIndices {
		UINT
			GraphicsSettings = ~0u,
			Camera = ~0u,
			SceneData = ~0u,
			InstanceData = ~0u,
			ObjectResourceDescriptorHeapIndices = ~0u, ObjectData = ~0u,
			Motions = ~0u,
			NormalRoughness = ~0u,
			ViewZ = ~0u,
			BaseColorMetalness = ~0u,
			NoisyDiffuse = ~0u, NoisySpecular = ~0u,
			Output = ~0u,
			EnvironmentLightCubeMap = ~0u, EnvironmentCubeMap = ~0u;
		UINT _;
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
		struct { UINT Vertices = ~0u, Indices = ~0u, Motions = ~0u; } Mesh;
		struct {
			UINT BaseColorMap = ~0u, EmissiveColorMap = ~0u, MetallicMap = ~0u, RoughnessMap = ~0u, AmbientOcclusionMap = ~0u, TransmissionMap = ~0u, OpacityMap = ~0u, NormalMap = ~0u;
		} Textures;
	};

	struct ObjectData {
		Material Material;
		XMFLOAT4X4 TextureTransform;
	};
}
