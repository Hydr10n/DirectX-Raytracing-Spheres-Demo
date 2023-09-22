/*
Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.

NVIDIA CORPORATION and its licensors retain all intellectual property
and proprietary rights in and to this software, related documentation
and any modifications thereto. Any use, reproduction, disclosure or
distribution of this software and related documentation without an express
license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#define NRD_HEADER_ONLY
#include "NRDEncoding.hlsli"
#include "NRD.hlsli"

#include "Math.hlsli"

SamplerState gLinearSampler : register( s0 );

Texture2D<float3> gIn_HistoryOutput : register( t0 );
Texture2D<float4> gIn_CurrentOutput : register( t1 );
Texture2D<float3> gIn_MotionVectors : register( t2 );

RWTexture2D<float3> gOut_FinalOutput : register( u0 );

cbuffer _ : register(b0) { uint2 gRectSize; }

cbuffer Data : register( b1 )
{
    bool gReset;
    float3 gCameraPosition;
    float3 gCameraRightDirection;
    float _;
    float3 gCameraUpDirection;
    float _1;
    float3 gCameraForwardDirection;
    float _2;
    float4x4 gCameraWorldToClipPrev;
}

#define ROOT_SIGNATURE \
    "StaticSampler( s0, filter=FILTER_MIN_MAG_MIP_LINEAR, addressU=TEXTURE_ADDRESS_CLAMP, addressV=TEXTURE_ADDRESS_CLAMP )," \
    "DescriptorTable( SRV( t0 ) )," \
    "DescriptorTable( SRV( t1 ) )," \
    "DescriptorTable( SRV( t2 ) )," \
    "DescriptorTable( UAV( u0 ) )," \
	"RootConstants( num32BitConstants=2, b0 )," \
    "CBV( b1 )"

#define BORDER          1
#define GROUP_X         16
#define GROUP_Y         16
#define BUFFER_X        ( GROUP_X + BORDER * 2 )
#define BUFFER_Y        ( GROUP_Y + BORDER * 2 )

groupshared float4 s_Data[ BUFFER_Y ][ BUFFER_X ];

void Preload( uint2 sharedPos, int2 globalPos, uint2 rectSize )
{
    globalPos = clamp( globalPos, 0, rectSize - 1.0 );

    float4 color_viewZ = gIn_CurrentOutput[ globalPos ];
    color_viewZ.w = color_viewZ.w / NRD_FP16_VIEWZ_SCALE;

    s_Data[ sharedPos.y ][ sharedPos.x ] = color_viewZ;
}

#define TAA_HISTORY_SHARPNESS               0.5 // [0; 1], 0.5 matches Catmull-Rom
#define TAA_MOTION_MAX_REUSE                0.1
#define TAA_MAX_HISTORY_WEIGHT              0.95
#define TAA_MIN_HISTORY_WEIGHT              0.1

float3 BicubicFilterNoCorners( Texture2D<float3> tex, SamplerState samp, float2 samplePos, float2 invTextureSize, compiletime const float sharpness )
{
    float2 centerPos = floor( samplePos - 0.5 ) + 0.5;
    float2 f = samplePos - centerPos;
    float2 f2 = f * f;
    float2 f3 = f * f2;
    float2 w0 = -sharpness * f3 + 2.0 * sharpness * f2 - sharpness * f;
    float2 w1 = ( 2.0 - sharpness ) * f3 - ( 3.0 - sharpness ) * f2 + 1.0;
    float2 w2 = -( 2.0 - sharpness ) * f3 + ( 3.0 - 2.0 * sharpness ) * f2 + sharpness * f;
    float2 w3 = sharpness * f3 - sharpness * f2;
    float2 wl2 = w1 + w2;
    float2 tc2 = invTextureSize * ( centerPos + w2 * STL::Math::PositiveRcp( wl2 ) );
    float2 tc0 = invTextureSize * ( centerPos - 1.0 );
    float2 tc3 = invTextureSize * ( centerPos + 2.0 );

    float w = wl2.x * w0.y;
    float3 color = tex.SampleLevel( samp, float2( tc2.x, tc0.y ), 0 ) * w;
    float sum = w;

    w = w0.x  * wl2.y;
    color += tex.SampleLevel( samp, float2( tc0.x, tc2.y ), 0 ) * w;
    sum += w;

    w = wl2.x * wl2.y;
    color += tex.SampleLevel( samp, float2( tc2.x, tc2.y ), 0 ) * w;
    sum += w;

    w = w3.x  * wl2.y;
    color += tex.SampleLevel( samp, float2( tc3.x, tc2.y ), 0 ) * w;
    sum += w;

    w = wl2.x * w3.y;
    color += tex.SampleLevel( samp, float2( tc2.x, tc3.y ), 0 ) * w;
    sum += w;

    color *= STL::Math::PositiveRcp( sum );

    return color;
}

[RootSignature( ROOT_SIGNATURE )]
[numthreads( GROUP_X, GROUP_Y, 1 )]
void main( int2 threadPos : SV_GroupThreadId, int2 pixelPos : SV_DispatchThreadId, uint threadIndex : SV_GroupIndex )
{
    int2 groupBase = pixelPos - threadPos - BORDER;
    uint stageNum = ( BUFFER_X * BUFFER_Y + GROUP_X * GROUP_Y - 1 ) / ( GROUP_X * GROUP_Y );
    [unroll]
    for( uint stage = 0; stage < stageNum; stage++ )
    {
        uint virtualIndex = threadIndex + stage * GROUP_X * GROUP_Y; \
        uint2 newId = uint2( virtualIndex % BUFFER_X, virtualIndex / BUFFER_X );
        if( stage == 0 || virtualIndex < BUFFER_X * BUFFER_Y )
            Preload( newId, groupBase + newId, gRectSize );
    }
    GroupMemoryBarrierWithGroupSync( );

    // Do not generate NANs for unused threads
    if( pixelPos.x >= gRectSize.x || pixelPos.y >= gRectSize.y )
        return;

    // Neighborhood
    float3 m1 = 0;
    float3 m2 = 0;
    float3 input = 0;

    float viewZ = s_Data[ threadPos.y + BORDER ][ threadPos.x + BORDER ].w;
    float viewZnearest = viewZ;
    int2 offseti = int2( BORDER, BORDER );

    [unroll]
    for( int dy = 0; dy <= BORDER * 2; dy++ )
    {
        [unroll]
        for( int dx = 0; dx <= BORDER * 2; dx++ )
        {
            int2 t = int2( dx, dy );
            int2 smemPos = threadPos + t;
            float4 data = s_Data[ smemPos.y ][ smemPos.x ];

            if( dx == BORDER && dy == BORDER )
                input = data.xyz;
            else
            {
                int2 t1 = t - BORDER;
                if( ( abs( t1.x ) + abs( t1.y ) == 1 ) && abs( data.w ) < abs( viewZnearest ) )
                {
                    viewZnearest = data.w;
                    offseti = t;
                }
            }

            m1 += data.xyz;
            m2 += data.xyz * data.xyz;
        }
    }

    float invSum = 1.0 / ( ( BORDER * 2 + 1 ) * ( BORDER * 2 + 1 ) );
    m1 *= invSum;
    m2 *= invSum;

    float3 sigma = sqrt( abs( m2 - m1 * m1 ) );

    float2 invRectSize = 1.0 / gRectSize;

    // Previous pixel position
    offseti -= BORDER;
    float2 offset = float2( offseti ) * invRectSize;
    float2 pixelUv = Math::CalculateUV( pixelPos, gRectSize ), NDC = Math::CalculateNDC( pixelUv + offset );
    float3 Xnearest = Math::CalculateWorldPosition( NDC, viewZnearest, gCameraPosition, gCameraRightDirection, gCameraUpDirection, gCameraForwardDirection );
    float3 mvNearest = gIn_MotionVectors[ pixelPos + offseti ] * invRectSize.xyy;
    float2 pixelUvPrev = STL::Geometry::GetPrevUvFromMotion( pixelUv + offset, Xnearest, gCameraWorldToClipPrev, mvNearest, STL_SCREEN_MOTION );
    pixelUvPrev -= offset;

    // History clamping
    float2 pixelPosPrev = saturate( pixelUvPrev ) * gRectSize;
    float3 history = gReset ? 0 : BicubicFilterNoCorners( gIn_HistoryOutput, gLinearSampler, pixelPosPrev, invRectSize, TAA_HISTORY_SHARPNESS ).xyz;
    float3 historyClamped = STL::Color::ClampAabb( m1, sigma, history );

    // History weight
    bool isInScreen = all( saturate( pixelUvPrev ) == pixelUvPrev );
    float2 pixelMotion = pixelUvPrev - pixelUv;
    float motionAmount = saturate( length( pixelMotion ) / TAA_MOTION_MAX_REUSE );
    float historyWeight = lerp( TAA_MAX_HISTORY_WEIGHT, TAA_MIN_HISTORY_WEIGHT, motionAmount );
    historyWeight *= float( isInScreen );

    // Final mix
    float3 result = lerp( input, historyClamped, historyWeight );

    // Output
    gOut_FinalOutput[ pixelPos ] = result;
}
