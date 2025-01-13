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
	bool IsStatic, IsEnvironmentLightTextureCubeMap;
	uint2 _;
	float4 EnvironmentLightColor;
	row_major float3x4 EnvironmentLightTextureTransform;
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
