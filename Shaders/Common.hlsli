#pragma once

#include "Material.hlsli"

struct SceneData {
	bool IsStatic, IsEnvironmentLightTextureCubeMap, IsEnvironmentTextureCubeMap;
	bool _;
	float4 EnvironmentLightColor;
	float4x4 EnvironmentLightTextureTransform;
	float4 EnvironmentColor;
	float4x4 EnvironmentTextureTransform;
};

struct InstanceData {
	bool IsStatic;
	float4x4 PreviousObjectToWorld;
};

struct ObjectResourceDescriptorHeapIndices {
	struct { uint Vertices, Indices, MotionVectors; } Mesh;
	struct { uint BaseColorMap, EmissiveColorMap, MetallicMap, RoughnessMap, AmbientOcclusionMap, TransmissionMap, OpacityMap, NormalMap; } Textures;
};

struct ObjectData {
	Material Material;
	float4x4 TextureTransform;
};
