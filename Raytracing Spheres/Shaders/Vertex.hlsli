#ifndef VERTEX_HLSLI
#define VERTEX_HLSLI

struct Vertex {
	float3 Position, Normal;
	float2 TextureCoordinate;
};

inline float2 VertexAttribute(float2 vertexAttributes[3], BuiltInTriangleIntersectionAttributes builtInTriangleIntersectionAttributes) {
	return vertexAttributes[0]
		+ builtInTriangleIntersectionAttributes.barycentrics.x * (vertexAttributes[1] - vertexAttributes[0])
		+ builtInTriangleIntersectionAttributes.barycentrics.y * (vertexAttributes[2] - vertexAttributes[0]);
}

inline float3 VertexAttribute(float3 vertexAttributes[3], BuiltInTriangleIntersectionAttributes builtInTriangleIntersectionAttributes) {
	return vertexAttributes[0]
		+ builtInTriangleIntersectionAttributes.barycentrics.x * (vertexAttributes[1] - vertexAttributes[0])
		+ builtInTriangleIntersectionAttributes.barycentrics.y * (vertexAttributes[2] - vertexAttributes[0]);
}

#endif
