#pragma once

#include "Material.hlsli"

#include "Vertex.hlsli"

struct SceneResourceDescriptorIndices
{
	uint EnvironmentLightTexture, EnvironmentTexture;
	uint2 _;
};

struct SceneData
{
	bool IsStatic, IsEnvironmentLightTextureCubeMap, IsEnvironmentTextureCubeMap;
	bool _;
	float4 EnvironmentLightColor, EnvironmentColor;
	row_major float3x4 EnvironmentLightTextureTransform, EnvironmentTextureTransform;
	SceneResourceDescriptorIndices ResourceDescriptorIndices;
};

struct InstanceData
{
	uint FirstGeometryIndex;
	uint3 _;
	row_major float3x4 PreviousObjectToWorld, ObjectToWorld;
};

struct MeshResourceDescriptorIndices
{
	uint Vertices, Indices, MotionVectors, _;
};

struct TextureMapResourceDescriptorIndices
{
	uint
		BaseColor,
		EmissiveColor,
		Metallic,
		Roughness,
		AmbientOcclusion,
		Transmission,
		Opacity,
		Normal;
};

struct ObjectResourceDescriptorIndices
{
	MeshResourceDescriptorIndices Mesh;
	TextureMapResourceDescriptorIndices TextureMaps;
};

struct ObjectData
{
	VertexDesc VertexDesc;
	Material Material;
	ObjectResourceDescriptorIndices ResourceDescriptorIndices;
};
