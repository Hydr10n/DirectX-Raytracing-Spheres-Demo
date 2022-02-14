#ifndef COMMON_HLSLI
#define COMMON_HLSLI

#include "Material.hlsli"

#define MAX_TRACE_RECURSION_DEPTH 8

SamplerState g_anisotropicWrap : register(s0);

RWTexture2D<float4> g_output : register(u0);

RaytracingAccelerationStructure g_scene : register(t0);

StructuredBuffer<VertexPositionNormalTexture> g_vertices : register(t1);
ByteAddressBuffer g_indices : register(t2);

Texture2D<float4> g_colorMap : register(t3);
Texture2D<float3> g_normalMap : register(t4);

TextureCube<float4> g_environmentCubeMap : register(t5);

struct SceneConstant {
	float4x4 ProjectionToWorld;
	float3 CameraPosition;
	uint AntiAliasingSampleCount, FrameCount;
	bool IsLeftHandedCoordinateSystem, IsEnvironmentCubeMapUsed;
};
ConstantBuffer<SceneConstant> g_sceneConstant : register(b0);

struct TextureFlags { enum { ColorMap = 0x1, NormalMap = 0x2 }; };

struct ObjectConstant {
	uint TextureFlags;
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
	"CBV(b0),"
	"DescriptorTable(SRV(t5))"
};

LocalRootSignature LocalRootSignature = {
	"DescriptorTable(SRV(t1)),"
	"DescriptorTable(SRV(t2)),"
	"DescriptorTable(SRV(t3)),"
	"DescriptorTable(SRV(t4)),"
	"CBV(b1)"
};

#endif
