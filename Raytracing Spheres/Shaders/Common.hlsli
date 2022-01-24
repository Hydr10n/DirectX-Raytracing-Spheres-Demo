#ifndef SHADER_HLSLI
#define SHADER_HLSLI

#include "Vertex.hlsli"

#include "Material.hlsli"

#define MAX_TRACE_RECURSION_DEPTH 10

typedef VertexPositionNormalTexture Vertex;

SamplerState g_anisotropicWrap : register(s0);

RWTexture2D<float4> g_output : register(u0);

RaytracingAccelerationStructure g_scene : register(t0);

StructuredBuffer<Vertex> g_vertices : register(t1);

ByteAddressBuffer g_indices : register(t2);

Texture2D<float4> g_imageTexture : register(t3);

struct SceneConstant {
	float4x4 ProjectionToWorld;
	float3 CameraPosition;
	uint AntiAliasingSampleCount;
	uint FrameCount;
};
ConstantBuffer<SceneConstant> g_sceneConstant : register(b0);

struct ObjectConstant {
	bool IsImageTextureUsed;
	float3 Padding;
	Material Material;
};
ConstantBuffer<ObjectConstant> g_objectConstant : register(b1);

RaytracingPipelineConfig RaytracingPipelineConfig = { MAX_TRACE_RECURSION_DEPTH };

RaytracingShaderConfig RaytracingShaderConfig = { sizeof(float4) * 2, sizeof(BuiltInTriangleIntersectionAttributes) };

GlobalRootSignature GlobalRootSignature = {
	"StaticSampler(s0),"
	"DescriptorTable(UAV(u0)),"
	"SRV(t0),"
	"CBV(b0)"
};

LocalRootSignature LocalRootSignature = {
	"DescriptorTable(SRV(t1), SRV(t2)),"
	"DescriptorTable(SRV(t3)),"
	"CBV(b1)"
};

#endif