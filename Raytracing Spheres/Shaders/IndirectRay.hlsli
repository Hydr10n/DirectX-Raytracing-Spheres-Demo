#pragma once

#include "Raytracing.hlsli"

struct IndirectRay {
	struct TraceResult {
		ScatterType ScatterType;
		uint InstanceIndex;
		VertexPositionNormal Vertex;
		float3 Irradiance;
		float HitDistance;
		struct {
			float3 BaseColor;
			float Metalness, Roughness;
		} Material;

		void Initialize() {
			ScatterType = ScatterType::DiffuseReflection;
			InstanceIndex = ~0u;
			Vertex = (VertexPositionNormal)0;
			Irradiance = 0;
			HitDistance = 1.#INFf;
			Material.BaseColor = 0;
			Material.Metalness = Material.Roughness = 0;
		}
	};

	static TraceResult Trace(RayDesc rayDesc) {
		TraceResult traceResult;
		traceResult.Initialize();

		float3 emissiveColor = 0, incidentColor = 0, attenuation = 1;

		RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
		for (uint depth = 0; ; depth++) {
			if (depth == g_globalData.MaxTraceRecursionDepth) {
				if (depth == 1) incidentColor = 1;

				break;
			}

			q.TraceRayInline(g_scene, RAY_FLAG_NONE, ~0u, rayDesc);
			q.Proceed();

			if (q.CommittedStatus() == COMMITTED_NOTHING) {
				if (!depth) {
					if (g_globalResourceDescriptorHeapIndices.EnvironmentCubeMap != ~0u) {
						incidentColor = g_environmentCubeMap.SampleLevel(g_anisotropicSampler, normalize(STL::Geometry::RotateVector(g_globalData.EnvironmentCubeMapTransform, rayDesc.Direction)), 0);
						break;
					}
					if (g_globalData.EnvironmentColor.a >= 0) {
						incidentColor = g_globalData.EnvironmentColor.rgb;
						break;
					}
				}

				if (g_globalResourceDescriptorHeapIndices.EnvironmentLightCubeMap != ~0u) {
					incidentColor = g_environmentLightCubeMap.SampleLevel(g_anisotropicSampler, normalize(STL::Geometry::RotateVector(g_globalData.EnvironmentLightCubeMapTransform, rayDesc.Direction)), 0);
				}
				else if (g_globalData.EnvironmentLightColor.a >= 0) incidentColor = g_globalData.EnvironmentLightColor.rgb;
				else incidentColor = lerp(1, float3(0.5f, 0.7f, 1), (rayDesc.Direction.y + 1) * 0.5f);

				break;
			}

			const uint instanceIndex = q.CommittedInstanceIndex();

			const float rayT = q.CommittedRayT();

			const HitInfo hitInfo = GetHitInfo(instanceIndex, q.WorldRayOrigin(), rayDesc.Direction, rayT, q.CommittedObjectToWorld3x4(), q.CommittedPrimitiveIndex(), q.CommittedTriangleBarycentrics());

			const Material material = GetMaterial(instanceIndex, hitInfo.Vertex.TextureCoordinate);

			const ScatterResult scatterResult = material.Scatter(hitInfo, rayDesc.Direction);

			emissiveColor += attenuation * material.EmissiveColor.rgb;
			attenuation *= scatterResult.Attenuation;

			if (!depth) {
				rayDesc.TMin = 1e-3f;
				rayDesc.TMax = 1.#INFf;

				traceResult.ScatterType = scatterResult.Type;
				traceResult.InstanceIndex = instanceIndex;
				traceResult.Vertex = hitInfo.Vertex;
				traceResult.Material.BaseColor = material.BaseColor.rgb;
				traceResult.Material.Metalness = material.Metallic;
				traceResult.Material.Roughness = material.Roughness;
			}
			else if (depth == 1) traceResult.HitDistance = rayT;
			else if (g_globalData.IsRussianRouletteEnabled && depth > 3) {
				const float probability = max(attenuation.r, max(attenuation.g, attenuation.b));
				if (STL::Rng::GetFloat2().x < probability) attenuation /= probability;
				else break;
			}

			rayDesc.Origin = hitInfo.Vertex.Position;
			rayDesc.Direction = scatterResult.Direction;
		}

		traceResult.Irradiance = emissiveColor + incidentColor * attenuation;

		return traceResult;
	}
};
