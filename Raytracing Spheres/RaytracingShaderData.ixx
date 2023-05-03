module;

#include "directxtk12/SimpleMath.h"

export module RaytracingShaderData;

import Material;

using namespace DirectX;

export {
	struct GlobalResourceDescriptorHeapIndices {
		UINT
			Camera = ~0u,
			GlobalData = ~0u,
			InstanceData = ~0u,
			ObjectResourceDescriptorHeapIndices = ~0u, ObjectData = ~0u,
			Motion = ~0u,
			NormalRoughness = ~0u,
			ViewZ = ~0u,
			BaseColorMetalness = ~0u,
			NoisyDiffuse = ~0u, NoisySpecular = ~0u,
			Output = ~0u,
			EnvironmentLightCubeMap = ~0u, EnvironmentCubeMap = ~0u;
		XMUINT2 _;
	};

	struct GlobalData {
		UINT FrameIndex, MaxTraceRecursionDepth, SamplesPerPixel;
		BOOL IsRussianRouletteEnabled, IsWorldStatic;
		XMUINT3 _;
		XMFLOAT4 NRDHitDistanceParameters, EnvironmentLightColor;
		XMFLOAT4X4 EnvironmentLightCubeMapTransform;
		XMFLOAT4 EnvironmentColor;
		XMFLOAT4X4 EnvironmentCubeMapTransform;
	};

	struct InstanceData {
		BOOL IsStatic;
		XMUINT3 _;
		XMFLOAT4X4 PreviousObjectToWorld;
	};

	struct ObjectResourceDescriptorHeapIndices {
		struct {
			UINT Vertices = ~0u, Indices = ~0u;
			XMUINT2 _;
		} TriangleMesh;
		struct {
			UINT BaseColorMap = ~0u, EmissiveColorMap = ~0u, MetallicMap = ~0u, RoughnessMap = ~0u, AmbientOcclusionMap = ~0u, TransmissionMap = ~0u, OpacityMap = ~0u, NormalMap = ~0u;
		} Textures;
	};

	struct ObjectData {
		Material Material;
		XMFLOAT4X4 TextureTransform;
	};
}
