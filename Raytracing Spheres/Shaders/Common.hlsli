#ifndef COMMON_HLSLI
#define COMMON_HLSLI

#include "Material.hlsli"

#include "Camera.hlsli"

#define MAX_TRACE_RECURSION_DEPTH 7

SamplerState g_anisotropicWrap : register(s0);

RWTexture2D<float4> g_output : register(u0);

RaytracingAccelerationStructure g_scene : register(t0);

StructuredBuffer<VertexPositionNormalTexture> g_vertices : register(t1);
ByteAddressBuffer g_indices : register(t2);

Texture2D<float4> g_colorMap : register(t3);
Texture2D<float3> g_normalMap : register(t4);

TextureCube<float4> g_environmentCubeMap : register(t5);

struct SceneConstants {
	uint RaytracingSamplesPerPixel, FrameCount;
	bool IsEnvironmentCubeMapUsed;
	float _padding;
	float4x4 EnvironmentMapTransform;
	Camera Camera;
};
ConstantBuffer<SceneConstants> g_sceneConstants : register(b0);

struct TextureFlags { enum { ColorMap = 0x1, NormalMap = 0x2 }; };
struct ObjectConstants {
	uint TextureFlags;
	float3 _padding;
	float4x4 TextureTransform;
	Material Material;
};
ConstantBuffer<ObjectConstants> g_objectConstants : register(b1);

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
