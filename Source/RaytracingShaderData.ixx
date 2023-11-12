module;

#include "directxtk12/SimpleMath.h"

export module RaytracingShaderData;

import Material;
import NRD;

using namespace DirectX;

export {
	struct GlobalResourceDescriptorHeapIndices {
		UINT
			InGraphicsSettings = ~0u,
			InCamera = ~0u,
			InSceneData = ~0u,
			InInstanceData = ~0u,
			InObjectResourceDescriptorHeapIndices = ~0u, InObjectData = ~0u,
			InEnvironmentLightTexture = ~0u, InEnvironmentTexture = ~0u,
			OutColor = ~0u,
			OutLinearDepth = ~0u, OutNormalizedDepth = ~0u,
			OutMotionVectors = ~0u,
			OutBaseColorMetalness = ~0u,
			OutEmissiveColor = ~0u,
			OutNormalRoughness = ~0u,
			OutNoisyDiffuse = ~0u, OutNoisySpecular = ~0u;
		XMUINT3 _;
	};

	struct GraphicsSettings {
		UINT FrameIndex, MaxNumberOfBounces, SamplesPerPixel;
		BOOL IsRussianRouletteEnabled;
		NRDDenoiser NRDDenoiser;
		XMUINT3 _;
		XMFLOAT4 NRDHitDistanceParameters;
	};

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
		struct {
			UINT BaseColorMap = ~0u, EmissiveColorMap = ~0u, MetallicMap = ~0u, RoughnessMap = ~0u, AmbientOcclusionMap = ~0u, TransmissionMap = ~0u, OpacityMap = ~0u, NormalMap = ~0u;
		} Textures;
	};

	struct ObjectData {
		Material Material;
		XMFLOAT4X4 TextureTransform;
	};
}
