#ifndef VERTEX_HLSLI
#define VERTEX_HLSLI

struct Vertex {
	static float2 Interpolate(float2 attributes[3], float2 barycentrics) {
		return attributes[0] + barycentrics.x * (attributes[1] - attributes[0]) + barycentrics.y * (attributes[2] - attributes[0]);
	}

	static float3 Interpolate(float3 attributes[3], float2 barycentrics) {
		return attributes[0] + barycentrics.x * (attributes[1] - attributes[0]) + barycentrics.y * (attributes[2] - attributes[0]);
	}
};

struct VertexPosition : Vertex { float3 Position; };

struct VertexPositionNormal : VertexPosition { float3 Normal; };

struct VertexPositionNormalTexture : VertexPositionNormal { float2 TextureCoordinate; };

#endif
