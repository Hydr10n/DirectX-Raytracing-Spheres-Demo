#pragma once

#include "Material.hlsli"

#include "Vertex.hlsli"

struct SceneResourceDescriptorHeapIndices {
	uint InEnvironmentLightTexture, InEnvironmentTexture;
	uint2 _;
};

struct SceneData {
	bool IsStatic, IsEnvironmentLightTextureCubeMap, IsEnvironmentTextureCubeMap;
	bool _;
	SceneResourceDescriptorHeapIndices ResourceDescriptorHeapIndices;
	float4 EnvironmentLightColor, EnvironmentColor;
	row_major float3x4 EnvironmentLightTextureTransform, EnvironmentTextureTransform;
};

struct InstanceData {
	uint FirstGeometryIndex;
	uint3 _;
	row_major float3x4 PreviousObjectToWorld, ObjectToWorld;
};

struct ObjectResourceDescriptorHeapIndices {
	struct { uint Vertices, Indices, MotionVectors, _; } Mesh;
	struct { uint BaseColorMap, EmissiveColorMap, MetallicMap, RoughnessMap, AmbientOcclusionMap, TransmissionMap, OpacityMap, NormalMap; } Textures;
};

struct ObjectData {
	VertexDesc VertexDesc;
	Material Material;
	ObjectResourceDescriptorHeapIndices ResourceDescriptorHeapIndices;
};
