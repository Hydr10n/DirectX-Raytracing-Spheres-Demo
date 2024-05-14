#pragma once

#include "Material.hlsli"

#include "Vertex.hlsli"

#include "Denoiser.hlsli"

struct SceneResourceDescriptorIndices {
	uint InEnvironmentLightTexture, InEnvironmentTexture;
	uint2 _;
};

struct SceneData {
	bool IsStatic, IsEnvironmentLightTextureCubeMap, IsEnvironmentTextureCubeMap;
	bool _;
	SceneResourceDescriptorIndices ResourceDescriptorIndices;
	float4 EnvironmentLightColor, EnvironmentColor;
	row_major float3x4 EnvironmentLightTextureTransform, EnvironmentTextureTransform;
};

struct InstanceData {
	uint FirstGeometryIndex;
	uint3 _;
	row_major float3x4 PreviousObjectToWorld, ObjectToWorld;
};

struct ObjectResourceDescriptorIndices {
	struct { uint Vertices, Indices, MotionVectors, _; } Mesh;
	struct { uint BaseColor, EmissiveColor, Metallic, Roughness, AmbientOcclusion, Transmission, Opacity, Normal; } TextureMaps;
};

struct ObjectData {
	VertexDesc VertexDesc;
	Material Material;
	ObjectResourceDescriptorIndices ResourceDescriptorIndices;
};

struct NRDSettings {
	NRDDenoiser Denoiser;
	uint3 _;
	float4 HitDistanceParameters;
};
