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
			Output = ~0u,
			EnvironmentLightCubeMap = ~0u, EnvironmentCubeMap = ~0u;
	};

	struct InstanceResourceDescriptorHeapIndices {
		struct {
			UINT Vertices = ~0u, Indices = ~0u;
			XMUINT2 _;
		} TriangleMesh;
		struct {
			UINT BaseColorMap = ~0u, EmissiveColorMap = ~0u, SpecularMap = ~0u, MetallicMap = ~0u, RoughnessMap = ~0u, AmbientOcclusionMap = ~0u, OpacityMap = ~0u, NormalMap = ~0u;
		} Textures;
	};

	struct GlobalData {
		UINT FrameIndex, AccumulatedFrameIndex, RaytracingMaxTraceRecursionDepth, RaytracingSamplesPerPixel;
		float MaterialBaseColorAlphaThreshold;
		XMFLOAT3 _;
		XMFLOAT4 EnvironmentLightColor;
		Matrix EnvironmentLightCubeMapTransform;
		XMFLOAT4 EnvironmentColor;
		Matrix EnvironmentCubeMapTransform;
	};

	struct InstanceData {
		Material Material;
		Matrix TextureTransform;
	};
}
