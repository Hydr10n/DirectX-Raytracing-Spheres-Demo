#pragma once

namespace ColorHelpers {
	inline float3 RGBToYCgCo(float3 rgb) {
		return float3(dot(rgb, float3(0.25f, 0.50f, 0.25f)), dot(rgb, float3(-0.25f, 0.50f, -0.25f)), dot(rgb, float3(0.50f, 0.00f, -0.50f)));
	}
	
	inline float3 YCgCoToRGB(float3 YCgCo) {
		const float temp = YCgCo.x - YCgCo.y;
		return float3(temp + YCgCo.z, YCgCo.x + YCgCo.y, temp - YCgCo.z);
	}
}
