#include "STL.hlsli"

#include "Math.hlsli"

RWTexture2D<float> g_depth : register(u0);

cbuffer _ : register(b0) { uint2 g_renderSize; }

cbuffer Data : register(b1) {
	float3 g_cameraPosition;
	float _;
	float3 g_cameraRightDirection;
	float _1;
	float3 g_cameraUpDirection;
	float _2;
	float3 g_cameraForwardDirection;
	float _3;
	float4x4 g_cameraWorldToProjection;
}

#define ROOT_SIGNATURE \
	"DescriptorTable(UAV(u0))," \
	"RootConstants(num32BitConstants=2, b0)," \
	"CBV(b1)"

[RootSignature(ROOT_SIGNATURE)]
[numthreads(16, 16, 1)]
void main(uint2 pixelCoordinate : SV_DispatchThreadID) {
	if (pixelCoordinate.x >= g_renderSize.x || pixelCoordinate.y >= g_renderSize.y) return;

	const float2 NDC = Math::CalculateNDC(Math::CalculateUV(pixelCoordinate, g_renderSize));
	const float3 position = Math::CalculateWorldPosition(NDC, g_depth[pixelCoordinate], g_cameraPosition, g_cameraRightDirection, g_cameraUpDirection, g_cameraForwardDirection);
	const float4 projection = STL::Geometry::ProjectiveTransform(g_cameraWorldToProjection, position);
	g_depth[pixelCoordinate] = projection.z / projection.w;
}
