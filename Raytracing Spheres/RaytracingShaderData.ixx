module;

#include "directxtk12/SimpleMath.h"

export module RaytracingShaderData;

import Material;

using namespace DirectX;
using namespace DirectX::SimpleMath;

export {
	struct GlobalResourceDescriptorHeapIndices {
		UINT
			InstanceResourceDescriptorHeapIndices = ~0u,
			Camera = ~0u,
			GlobalData = ~0u, InstanceData = ~0u,
			Motion = ~0u,
			NormalRoughness = ~0u,
			ViewZ = ~0u,
			BaseColorMetalness = ~0u,
			NoisyDiffuse = ~0u, NoisySpecular = ~0u,
			Output = ~0u,
			EnvironmentLightCubeMap = ~0u, EnvironmentCubeMap = ~0u;
		XMUINT3 _;
	};

	struct InstanceResourceDescriptorHeapIndices {
		struct {
			UINT Vertices = ~0u, Indices = ~0u;
			XMUINT2 _;
		} TriangleMesh;
		struct {
			UINT BaseColorMap = ~0u, EmissiveColorMap = ~0u, MetallicMap = ~0u, RoughnessMap = ~0u, AmbientOcclusionMap = ~0u, OpacityMap = ~0u, NormalMap = ~0u;
			UINT _;
		} Textures;
	};

	struct GlobalData {
		UINT FrameIndex, MaxTraceRecursionDepth, SamplesPerPixel;
		BOOL IsRussianRouletteEnabled;
		XMFLOAT4 NRDHitDistanceParameters, EnvironmentLightColor;
		XMFLOAT4X4 EnvironmentLightCubeMapTransform;
		XMFLOAT4 EnvironmentColor;
		XMFLOAT4X4 EnvironmentCubeMapTransform;
	};

	struct InstanceData {
		float MaterialBaseColorAlphaThreshold;
		XMFLOAT3 _;
		Material Material;
		XMFLOAT4X4 TextureTransform, WorldToPreviousWorld;
	};
}
