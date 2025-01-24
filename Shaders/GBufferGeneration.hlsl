#include "Common.hlsli"

#include "Camera.hlsli"

#include "Denoiser.hlsli"

SamplerState g_anisotropicSampler : register(s0);

RaytracingAccelerationStructure g_scene : register(t0);

struct Flags {
	enum {
		Radiance = 0x1,
		Position = 0x2,
		LinearDepth = 0x4,
		NormalizedDepth = 0x8,
		MotionVector = 0x10,
		FlatNormal = 0x20,
		Normals = 0x40,
		NormalRoughness = 0x80,
		Geometry = Position | LinearDepth | NormalizedDepth | MotionVector | FlatNormal | Normals | NormalRoughness,
		Material = Radiance | NormalRoughness | 0x100
	};
};

struct Constants {
	uint2 RenderSize;
	uint Flags;
};
ConstantBuffer<Constants> g_constants : register(b0);

ConstantBuffer<SceneData> g_sceneData : register(b1);

ConstantBuffer<Camera> g_camera : register(b2);

StructuredBuffer<InstanceData> g_instanceData : register(t1);
StructuredBuffer<ObjectData> g_objectData : register(t2);

RWTexture2D<float3> g_Radiance : register(u0);
RWTexture2D<float4> g_Position : register(u1);
RWTexture2D<float> g_LinearDepth : register(u2);
RWTexture2D<float> g_NormalizedDepth : register(u3);
RWTexture2D<float3> g_MotionVector : register(u4);
RWTexture2D<float4> g_BaseColorMetalness : register(u5);
RWTexture2D<float2> g_FlatNormal : register(u6);
RWTexture2D<float4> g_Normals : register(u7);
RWTexture2D<float> g_Roughness : register(u8);
RWTexture2D<float4> g_NormalRoughness : register(u9);
RWTexture2D<float> g_Transmission : register(u10);
RWTexture2D<float> g_IOR : register(u11);

#include "RaytracingHelpers.hlsli"

#define SET(Name) g_##Name[pixelPosition] = Name;
#define SET1(Name) if (g_constants.Flags & Flags::Name) { SET(Name); }

float3 CalculateMotionVector(float2 UV, uint2 pixelDimensions, float linearDepth, HitInfo hitInfo)
{
	float3 previousPosition;
	if (g_sceneData.IsStatic)
	{
		previousPosition = hitInfo.Position;
	}
	else
	{
		previousPosition = hitInfo.ObjectPosition;
		const MeshResourceDescriptorIndices meshIndices = g_objectData[hitInfo.ObjectIndex].ResourceDescriptorIndices.Mesh;
		if (meshIndices.MotionVectors != ~0u)
		{
			const StructuredBuffer<float3> meshMotionVectors = ResourceDescriptorHeap[meshIndices.MotionVectors];
			const uint3 indices = MeshHelpers::Load3Indices(ResourceDescriptorHeap[meshIndices.Indices], hitInfo.PrimitiveIndex);
			const float3 motionVectors[] = { meshMotionVectors[indices[0]], meshMotionVectors[indices[1]], meshMotionVectors[indices[2]] };
			previousPosition += Vertex::Interpolate(motionVectors, hitInfo.Barycentrics);
		}
		previousPosition = Geometry::AffineTransform(g_instanceData[hitInfo.InstanceIndex].PreviousObjectToWorld, previousPosition);
	}
	return float3((Geometry::GetScreenUv(g_camera.PreviousWorldToProjection, previousPosition) - UV) * pixelDimensions, Geometry::AffineTransform(g_camera.PreviousWorldToView, previousPosition).z - linearDepth);
}

[RootSignature(
	"RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED),"
	"StaticSampler(s0),"
	"SRV(t0),"
	"RootConstants(num32BitConstants=3, b0),"
	"CBV(b1),"
	"CBV(b2),"
	"SRV(t1),"
	"SRV(t2),"
	"DescriptorTable(UAV(u0)),"
	"DescriptorTable(UAV(u1)),"
	"DescriptorTable(UAV(u2)),"
	"DescriptorTable(UAV(u3)),"
	"DescriptorTable(UAV(u4)),"
	"DescriptorTable(UAV(u5)),"
	"DescriptorTable(UAV(u6)),"
	"DescriptorTable(UAV(u7)),"
	"DescriptorTable(UAV(u8)),"
	"DescriptorTable(UAV(u9)),"
	"DescriptorTable(UAV(u10)),"
	"DescriptorTable(UAV(u11))"
)]
[numthreads(16, 16, 1)]
void main(uint2 pixelPosition : SV_DispatchThreadID)
{
	const uint2 pixelDimensions = g_constants.RenderSize;
	if (any(pixelPosition >= pixelDimensions))
	{
		return;
	}

	float3 Radiance = 0;
	float LinearDepth = 1.#INF, NormalizedDepth = !g_camera.IsNormalizedDepthReversed;
	float3 MotionVector = 0;
	float4 NormalRoughness = 0;

	const float2 UV = Math::CalculateUV(pixelPosition, pixelDimensions, g_camera.Jitter);
	const RayDesc rayDesc = g_camera.GeneratePinholeRay(Math::CalculateNDC(UV));
	HitInfo hitInfo;
	if (CastRay(rayDesc, hitInfo))
	{
		if (g_constants.Flags & Flags::Geometry)
		{
			const float4
				Position = float4(hitInfo.Position, hitInfo.PositionOffset),
				projection = Geometry::ProjectiveTransform(g_camera.WorldToProjection, hitInfo.Position);
			LinearDepth = projection.w;
			NormalizedDepth = projection.z / projection.w;
			MotionVector = CalculateMotionVector(UV, pixelDimensions, LinearDepth, hitInfo);
			const float2 FlatNormal = Packing::EncodeUnitVector(hitInfo.FlatNormal, true);
			const float4 Normals = float4(Packing::EncodeUnitVector(hitInfo.Normal, true), Packing::EncodeUnitVector(hitInfo.GeometricNormal, true));

			SET1(Position);
			SET1(FlatNormal);
			SET1(Normals);
		}

		Material material;
		if (g_constants.Flags & Flags::Material)
		{
			bool hasSampledTexture;
			material = GetMaterial(g_objectData[hitInfo.ObjectIndex], hitInfo.TextureCoordinate, hasSampledTexture);

			Radiance = material.GetEmission();
			const float4 BaseColorMetalness = float4(material.BaseColor.rgb, material.Metallic);
			const float
				Roughness = material.Roughness,
				Transmission = material.Transmission,
				IOR = material.IOR;

			SET(BaseColorMetalness);
			SET(Roughness);

			if (BaseColorMetalness.a < 1)
			{
				SET(Transmission);
				SET(IOR);
			}
		}

		if (g_constants.Flags & Flags::NormalRoughness)
		{
			NormalRoughness = NRD_FrontEnd_PackNormalAndRoughness(hitInfo.Normal, material.Roughness, material.Metallic >= 0.5f);
		}
	}
	else if (g_constants.Flags & Flags::Material)
	{
		Radiance = GetEnvironmentLightColor(g_sceneData, rayDesc.Direction);
	}

	SET1(Radiance);
	SET1(LinearDepth);
	SET1(NormalizedDepth);
	SET1(MotionVector);
	SET1(NormalRoughness);
}
