#pragma once

struct Vertex {
	template <typename T>
	static T Interpolate(T attributes[3], float2 barycentrics) {
		return attributes[0] + barycentrics.x * (attributes[1] - attributes[0]) + barycentrics.y * (attributes[2] - attributes[0]);
	}
};

struct VertexPosition : Vertex { float3 Position; };

struct VertexPositionNormal : VertexPosition { float3 Normal; };

struct VertexPositionNormalTexture : VertexPositionNormal { float2 TextureCoordinate; };
