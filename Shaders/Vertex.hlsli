#pragma once

struct VertexDesc {
	uint Stride, NormalOffset, TextureCoordinateOffset, TangentOffset;

	float3 LoadPosition(ByteAddressBuffer buffer, uint index) { return asfloat(buffer.Load3(Stride * index)); }
	void LoadPositions(ByteAddressBuffer buffer, uint3 indices, out float3 attributes[3]) {
		attributes[0] = LoadPosition(buffer, indices[0]);
		attributes[1] = LoadPosition(buffer, indices[1]);
		attributes[2] = LoadPosition(buffer, indices[2]);
	}

	float3 LoadNormal(ByteAddressBuffer buffer, uint index) { return asfloat(buffer.Load3(Stride * index + NormalOffset)); }
	void LoadNormals(ByteAddressBuffer buffer, uint3 indices, out float3 attributes[3]) {
		attributes[0] = LoadNormal(buffer, indices[0]);
		attributes[1] = LoadNormal(buffer, indices[1]);
		attributes[2] = LoadNormal(buffer, indices[2]);
	}

	float2 LoadTextureCoordinate(ByteAddressBuffer buffer, uint index) { return asfloat(buffer.Load2(Stride * index + TextureCoordinateOffset)); }
	void LoadTextureCoordinates(ByteAddressBuffer buffer, uint3 indices, out float2 attributes[3]) {
		attributes[0] = LoadTextureCoordinate(buffer, indices[0]);
		attributes[1] = LoadTextureCoordinate(buffer, indices[1]);
		attributes[2] = LoadTextureCoordinate(buffer, indices[2]);
	}

	float3 LoadTangent(ByteAddressBuffer buffer, uint index) { return asfloat(buffer.Load3(Stride * index + TangentOffset)); }
	void LoadTangents(ByteAddressBuffer buffer, uint3 indices, out float3 attributes[3]) {
		attributes[0] = LoadTangent(buffer, indices[0]);
		attributes[1] = LoadTangent(buffer, indices[1]);
		attributes[2] = LoadTangent(buffer, indices[2]);
	}
};

struct Vertex {
	template <typename T>
	static T Interpolate(T attributes[3], float2 barycentrics) {
		return attributes[0] + barycentrics.x * (attributes[1] - attributes[0]) + barycentrics.y * (attributes[2] - attributes[0]);
	}
};

struct VertexPosition : Vertex { float3 Position; };

struct VertexPositionNormal : VertexPosition { float3 Normal; };

struct VertexPositionNormalTexture : VertexPositionNormal { float2 TextureCoordinate; };
