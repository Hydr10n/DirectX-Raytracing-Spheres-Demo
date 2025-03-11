#include "Common.hlsli"

#include "Camera.hlsli"

SamplerState g_anisotropicSampler : register(s0);

RaytracingAccelerationStructure g_scene : register(t0);

struct Flags
{
	enum
	{
		Position = 0x1,
		FlatNormal = 0x2,
		GeometricNormal = 0x4,
		LinearDepth = 0x8,
		NormalizedDepth = 0x10,
		MotionVector = 0x20,
		DiffuseAlbedo = 0x40,
		SpecularAlbedo = 0x80,
		Albedo = DiffuseAlbedo | SpecularAlbedo,
		NormalRoughness = 0x100,
		Radiance = 0x200,
		Geometry = Position | FlatNormal | GeometricNormal | LinearDepth | NormalizedDepth | MotionVector | NormalRoughness,
		Material = 0x400 | Albedo | NormalRoughness | Radiance
	};
};

struct Constants
{
	uint2 RenderSize;
	uint Flags;
};
ConstantBuffer<Constants> g_constants : register(b0);

ConstantBuffer<SceneData> g_sceneData : register(b1);

ConstantBuffer<Camera> g_camera : register(b2);

StructuredBuffer<InstanceData> g_instanceData : register(t1);
StructuredBuffer<ObjectData> g_objectData : register(t2);

RWTexture2D<float4> g_Position : register(u0);
RWTexture2D<float2> g_FlatNormal : register(u1);
RWTexture2D<float2> g_GeometricNormal : register(u2);
RWTexture2D<float> g_LinearDepth : register(u3);
RWTexture2D<float> g_NormalizedDepth : register(u4);
RWTexture2D<float3> g_MotionVector : register(u5);
RWTexture2D<float4> g_BaseColorMetalness : register(u6);
RWTexture2D<float3> g_DiffuseAlbedo : register(u7);
RWTexture2D<float3> g_SpecularAlbedo : register(u8);
RWTexture2D<float4> g_NormalRoughness : register(u9);
RWTexture2D<float> g_IOR : register(u10);
RWTexture2D<float> g_Transmission : register(u11);
RWTexture2D<float3> g_Radiance : register(u12);

#include "RaytracingHelpers.hlsli"

#define SET(Name) g_##Name[pixelPosition] = Name;
#define SET1(Name) if (g_constants.Flags & Flags::Name) { SET(Name); }

float3 CalculateMotionVector(float2 UV, uint2 pixelDimensions, float linearDepth, HitInfo hitInfo)
{
	float3 previousPosition;
	if (!isfinite(hitInfo.Distance) || g_sceneData.IsStatic)
	{
		previousPosition = hitInfo.Position;
	}
	else
	{
		previousPosition = hitInfo.ObjectPosition;
		const MeshDescriptors meshDescriptors = g_objectData[hitInfo.ObjectIndex].MeshDescriptors;
		if (meshDescriptors.MotionVectors != ~0u)
		{
			const StructuredBuffer<float16_t4> meshMotionVectors = ResourceDescriptorHeap[meshDescriptors.MotionVectors];
			const uint3 indices = MeshHelpers::Load3Indices(ResourceDescriptorHeap[meshDescriptors.Indices], hitInfo.PrimitiveIndex);
			const float3 motionVectors[] =
			{
				meshMotionVectors[indices[0]].xyz,
				meshMotionVectors[indices[1]].xyz,
				meshMotionVectors[indices[2]].xyz
			};
			previousPosition += Vertex::Interpolate(motionVectors, hitInfo.Barycentrics);
		}
		previousPosition = Geometry::AffineTransform(g_instanceData[hitInfo.InstanceIndex].PreviousObjectToWorld, previousPosition);
	}
	return float3(
		(Geometry::GetScreenUv(g_camera.PreviousWorldToProjection, previousPosition) - UV) * pixelDimensions,
		Geometry::AffineTransform(g_camera.PreviousWorldToView, previousPosition).z - linearDepth
	);
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
	"DescriptorTable(UAV(u11)),"
	"DescriptorTable(UAV(u12))"
)]
[numthreads(16, 16, 1)]
void main(uint2 pixelPosition : SV_DispatchThreadID)
{
	const uint2 pixelDimensions = g_constants.RenderSize;
	if (any(pixelPosition >= pixelDimensions))
	{
		return;
	}

	float LinearDepth = 1.#INF, NormalizedDepth = !g_camera.IsNormalizedDepthReversed;

	const float2 UV = Math::CalculateUV(pixelPosition, pixelDimensions, g_camera.Jitter);
	const RayDesc rayDesc = g_camera.GeneratePinholeRay(Math::CalculateNDC(UV));
	HitInfo hitInfo;
	if (CastRay(rayDesc, hitInfo))
	{
		if (g_constants.Flags & Flags::Geometry)
		{
			const float4 Position = float4(hitInfo.Position, hitInfo.PositionOffset);
			SET1(Position);

			const float2
				FlatNormal = Packing::EncodeUnitVector(hitInfo.FlatNormal, true),
				GeometricNormal = Packing::EncodeUnitVector(hitInfo.GeometricNormal, true);
			SET1(FlatNormal);
			SET1(GeometricNormal);

			const float4 projection = Geometry::ProjectiveTransform(g_camera.WorldToProjection, hitInfo.Position);
			LinearDepth = projection.w;
			NormalizedDepth = projection.z / projection.w;

			if (g_constants.Flags & Flags::MotionVector)
			{
				const float3 MotionVector = CalculateMotionVector(UV, pixelDimensions, LinearDepth, hitInfo);
				SET(MotionVector);
			}
		}

		BSDFSample BSDFSample;
		if (g_constants.Flags & Flags::Material)
		{
			const ObjectData objectData = g_objectData[hitInfo.ObjectIndex];

			bool hasSampledTexture;
			const Material material = EvaluateMaterial(
				hitInfo.ShadingNormal, hitInfo.GetFrontTangent(),
				objectData.Material,
				objectData.TextureMapInfoArray, hitInfo.TextureCoordinates,
				hasSampledTexture
			);
			BSDFSample.Initialize(material, hitInfo.IsFrontFace);

			const float4 BaseColorMetalness = float4(BSDFSample.BaseColor, BSDFSample.Metallic);
			SET(BaseColorMetalness);

			if (g_constants.Flags & Flags::Albedo)
			{
				float3 DiffuseAlbedo, SpecularAlbedo;
				SurfaceVectors surfaceVectors;
				surfaceVectors.Initialize(hitInfo.IsFrontFace, hitInfo.GeometricNormal, hitInfo.ShadingNormal);
				const float3 V = -rayDesc.Direction;
				BSDFSample.EstimateDemodulationFactors(surfaceVectors, V, DiffuseAlbedo, SpecularAlbedo);
				if (g_constants.Flags & Flags::DiffuseAlbedo)
				{
					SET(DiffuseAlbedo);
				}
				if (g_constants.Flags & Flags::SpecularAlbedo)
				{
					SET(SpecularAlbedo);
				}
			}

			const float IOR = material.IOR;
			SET(IOR);

			if (BSDFSample.Metallic < 1)
			{
				const float Transmission = BSDFSample.Transmission;
				SET(Transmission);
			}

			if (g_constants.Flags & Flags::Radiance)
			{
				const float3 Radiance = material.GetEmission();
				SET(Radiance);
			}
		}

		if (g_constants.Flags & Flags::NormalRoughness)
		{
			const float4 NormalRoughness = float4(
				hitInfo.ShadingNormal,
				g_constants.Flags & Flags::Material ? BSDFSample.Roughness : 0
			);
			SET(NormalRoughness);
		}
	}
	else
	{
		if (g_constants.Flags & Flags::MotionVector)
		{
			const float linearDepth = Geometry::ProjectiveTransform(g_camera.WorldToProjection, hitInfo.Position).w;
			const float3 MotionVector = CalculateMotionVector(UV, pixelDimensions, linearDepth, hitInfo);
			SET(MotionVector);
		}

		if (g_constants.Flags & Flags::Radiance)
		{
			const float3 Radiance = GetEnvironmentLightColor(g_sceneData, rayDesc.Direction);
			SET(Radiance);
		}
	}

	SET1(LinearDepth);
	SET1(NormalizedDepth);
}
