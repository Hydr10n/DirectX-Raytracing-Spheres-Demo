#pragma once

#include "Material.hlsli"

#include "Vertex.hlsli"

struct SceneData
{
	bool IsStatic, IsEnvironmentLightTextureCubeMap;
	uint EnvironmentLightTextureDescriptor, _;
	float4 EnvironmentLightColor;
	row_major float3x4 EnvironmentLightTransform;
};

struct InstanceData
{
	uint FirstGeometryIndex;
	uint3 _;
	row_major float3x4 PreviousObjectToWorld, ObjectToWorld;
};

struct MeshDescriptors
{
	uint Vertices, Indices, MotionVectors, _;
};

using TextureMapInfoArray = TextureMapInfo[TextureMapType::Count];

struct ObjectData
{
	VertexDesc VertexDesc;
	MeshDescriptors MeshDescriptors;
	Material Material;
	TextureMapInfoArray TextureMapInfoArray;
};
