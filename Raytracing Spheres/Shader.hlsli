#ifndef SHADER_HLSLI
#define SHADER_HLSLI

#include "Vertex.hlsli"

RaytracingAccelerationStructure g_scene : register(t0);

StructuredBuffer<Vertex> g_vertices : register(t1);
ByteAddressBuffer g_indices : register(t2);

Texture2D g_texture : register(t3);

struct SceneConstant {
	float4x4 ProjectionToWorld;
	float3 CameraPosition;
	uint MaxTraceRecursionDepth;
	uint AntiAliasingSampleCount;
	uint FrameCount;
};
ConstantBuffer<SceneConstant> g_sceneConstant : register(b0);

struct ObjectConstant { bool IsTextureUsed; };
ConstantBuffer<ObjectConstant> g_objectConstant : register(b1);

struct MaterialConstant {
	uint Type; // 0: Lambertian, 1: Metal, 2: Dielectric
	float RefractiveIndex; // Dielectric
	float Roughness; // Metal
	float Padding;
	float4 Color;
};
ConstantBuffer<MaterialConstant> g_materialConstant : register(b2);

#endif
