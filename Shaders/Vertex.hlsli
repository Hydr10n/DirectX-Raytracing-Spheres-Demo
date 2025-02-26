#pragma once

#include "Packing.hlsli"

struct VertexDesc
{
	uint Stride;
	uint3 _;
	struct
	{
		uint Normal, Tangent, TextureCoordinates[2];
	} AttributeOffsets;

	float3 LoadPosition(ByteAddressBuffer buffer, uint index)
	{
		return buffer.Load<float3>(Stride * index);
	}

	void LoadPositions(ByteAddressBuffer buffer, uint3 indices, out float3 attributes[3])
	{
		attributes[0] = LoadPosition(buffer, indices[0]);
		attributes[1] = LoadPosition(buffer, indices[1]);
		attributes[2] = LoadPosition(buffer, indices[2]);
	}

	float3 LoadNormal(ByteAddressBuffer buffer, uint index)
	{
		return Unpack_R16G16B16_SNORM(buffer.Load<int16_t3>(Stride * index + AttributeOffsets.Normal));
	}

	void LoadNormals(ByteAddressBuffer buffer, uint3 indices, out float3 attributes[3])
	{
		attributes[0] = LoadNormal(buffer, indices[0]);
		attributes[1] = LoadNormal(buffer, indices[1]);
		attributes[2] = LoadNormal(buffer, indices[2]);
	}

	float3 LoadTangent(ByteAddressBuffer buffer, uint index)
	{
		return Unpack_R16G16B16_SNORM(buffer.Load<int16_t3>(Stride * index + AttributeOffsets.Tangent));
	}

	void LoadTangents(ByteAddressBuffer buffer, uint3 indices, out float3 attributes[3])
	{
		attributes[0] = LoadTangent(buffer, indices[0]);
		attributes[1] = LoadTangent(buffer, indices[1]);
		attributes[2] = LoadTangent(buffer, indices[2]);
	}

	float2 LoadTextureCoordinate(ByteAddressBuffer buffer, uint index, uint index1)
	{
		return buffer.Load<float16_t2>(Stride * index + AttributeOffsets.TextureCoordinates[index1]);
	}

	void LoadTextureCoordinates(ByteAddressBuffer buffer, uint3 indices, uint index, out float2 attributes[3])
	{
		attributes[0] = LoadTextureCoordinate(buffer, indices[0], index);
		attributes[1] = LoadTextureCoordinate(buffer, indices[1], index);
		attributes[2] = LoadTextureCoordinate(buffer, indices[2], index);
	}
};

struct Vertex
{
	template <typename T>
	static T Interpolate(T attributes[3], float2 barycentrics)
	{
		return attributes[0]
			+ barycentrics.x * (attributes[1] - attributes[0])
			+ barycentrics.y * (attributes[2] - attributes[0]);
	}
};

struct VertexPositionNormalTangentTexture
{
	float3 Position;
	int16_t3 Normal, Tangent;
	float16_t2 TextureCoordinates[2];
};
