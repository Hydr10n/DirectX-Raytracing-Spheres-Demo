#pragma once

#include "Material.hlsli"

#include "Camera.hlsli"

#define UINT_MAX 0xffffffff

static const uint MaxTraceRecursionDepth = 7;

SamplerState g_anisotropicWrap : register(s0);

RaytracingAccelerationStructure g_scene : register(t0);

struct GlobalResourceDescriptorHeapIndices {
	uint
		LocalResourceDescriptorHeapIndices,
		Camera,
		GlobalData, LocalData,
		Output,
		EnvironmentCubeMap;
	uint2 _padding;
};
ConstantBuffer<GlobalResourceDescriptorHeapIndices> g_globalResourceDescriptorHeapIndices : register(b0);

struct LocalResourceDescriptorHeapIndices {
	struct {
		uint Vertices, Indices;
		uint2 _padding;
	} Mesh;
	struct {
		uint ColorMap, NormalMap;
		uint2 _padding;
	} Textures;
};
static const StructuredBuffer<LocalResourceDescriptorHeapIndices> g_localResourceDescriptorHeapIndices = ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.LocalResourceDescriptorHeapIndices];

static const ConstantBuffer<Camera> g_camera = ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.Camera];

struct GlobalData {
	uint RaytracingSamplesPerPixel, FrameCount;
	uint2 _padding;
	float4x4 EnvironmentMapTransform;
};
static const ConstantBuffer<GlobalData> g_globalData = ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.GlobalData];

struct LocalData {
	float4x4 TextureTransform;
	Material Material;
};
static const StructuredBuffer<LocalData> g_localData = ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.LocalData];

static const RWTexture2D<float4> g_output = ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.Output];

static const TextureCube<float4> g_environmentCubeMap = ResourceDescriptorHeap[g_globalResourceDescriptorHeapIndices.EnvironmentCubeMap];

RaytracingPipelineConfig RaytracingPipelineConfig = { MaxTraceRecursionDepth };

RaytracingShaderConfig RaytracingShaderConfig = { sizeof(float4) * 2, sizeof(BuiltInTriangleIntersectionAttributes) };

GlobalRootSignature GlobalRootSignature = {
	"RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED),"
	"StaticSampler(s0),"
	"SRV(t0),"
	"CBV(b0)"
};
