#ifndef VERTEXTYPES_HLSLI
#define VERTEXTYPES_HLSLI

struct VertexPosition { float3 Position; };

struct VertexPositionNormal : VertexPosition { float3 Normal; };

struct VertexPositionNormalTexture : VertexPositionNormal { float2 TextureCoordinate; };

#endif
