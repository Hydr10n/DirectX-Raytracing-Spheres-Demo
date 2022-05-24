/***************************************************************************
 # Copyright (c) 2015-21, NVIDIA CORPORATION. All rights reserved.
 #
 # Redistribution and use in source and binary forms, with or without
 # modification, are permitted provided that the following conditions
 # are met:
 #  * Redistributions of source code must retain the above copyright
 #    notice, this list of conditions and the following disclaimer.
 #  * Redistributions in binary form must reproduce the above copyright
 #    notice, this list of conditions and the following disclaimer in the
 #    documentation and/or other materials provided with the distribution.
 #  * Neither the name of NVIDIA CORPORATION nor the names of its
 #    contributors may be used to endorse or promote products derived
 #    from this software without specific prior written permission.
 #
 # THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS "AS IS" AND ANY
 # EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 # IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 # PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 # CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 # EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 # PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 # PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 # OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 # (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 # OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 **************************************************************************/
 #define ROOT_SIGNATURE \
    "StaticSampler(s0, filter=FILTER_MIN_MAG_MIP_LINEAR)," \
    "DescriptorTable(SRV(t0))," \
    "DescriptorTable(SRV(t1))," \
    "DescriptorTable(SRV(t2))," \
    "DescriptorTable(UAV(u0))," \
    "RootConstants(num32BitConstants=2, b0)"

SamplerState gSampler : register(s0);
Texture2D<float4> gTexPrevColor : register(t0);
Texture2D<float4> gTexColor : register(t1);
Texture2D<float2> gTexMotionVec : register(t2);
RWTexture2D<float4> gOutput : register(u0);

cbuffer PerFrameCB : register(b0)
{
    float gAlpha;
    float gColorBoxSigma;
}


/** Converts color from RGB to YCgCo space
    \param RGBColor linear HDR RGB color
*/
inline float3 RGBToYCgCo(float3 rgb)
{
    float Y = dot(rgb, float3(0.25f, 0.50f, 0.25f));
    float Cg = dot(rgb, float3(-0.25f, 0.50f, -0.25f));
    float Co = dot(rgb, float3(0.50f, 0.00f, -0.50f));
    return float3(Y, Cg, Co);
}

/** Converts color from YCgCo to RGB space
    \param YCgCoColor linear HDR YCgCo color
*/
inline float3 YCgCoToRGB(float3 YCgCo)
{
    float tmp = YCgCo.x - YCgCo.y;
    float r = tmp + YCgCo.z;
    float g = YCgCo.x + YCgCo.y;
    float b = tmp - YCgCo.z;
    return float3(r, g, b);
}


// Catmull-Rom filtering code from http://vec3.ca/bicubic-filtering-in-fewer-taps/
float3 bicubicSampleCatmullRom(Texture2D tex, SamplerState samp, float2 samplePos, float2 texDim)
{
    float2 invTextureSize = 1.0 / texDim;
    float2 tc = floor(samplePos - 0.5f) + 0.5f;
    float2 f = samplePos - tc;
    float2 f2 = f * f;
    float2 f3 = f2 * f;

    float2 w0 = f2 - 0.5f * (f3 + f);
    float2 w1 = 1.5f * f3 - 2.5f * f2 + 1.f;
    float2 w3 = 0.5f * (f3 - f2);
    float2 w2 = 1 - w0 - w1 - w3;

    float2 w12 = w1 + w2;

    float2 tc0 = (tc - 1.f) * invTextureSize;
    float2 tc12 = (tc + w2 / w12) * invTextureSize;
    float2 tc3 = (tc + 2.f) * invTextureSize;

    float3 result =
        tex.SampleLevel(samp, float2(tc0.x,  tc0.y), 0.f).rgb  * (w0.x  * w0.y) +
        tex.SampleLevel(samp, float2(tc0.x,  tc12.y), 0.f).rgb * (w0.x  * w12.y) +
        tex.SampleLevel(samp, float2(tc0.x,  tc3.y), 0.f).rgb  * (w0.x  * w3.y) +
        tex.SampleLevel(samp, float2(tc12.x, tc0.y), 0.f).rgb  * (w12.x * w0.y) +
        tex.SampleLevel(samp, float2(tc12.x, tc12.y), 0.f).rgb * (w12.x * w12.y) +
        tex.SampleLevel(samp, float2(tc12.x, tc3.y), 0.f).rgb  * (w12.x * w3.y) +
        tex.SampleLevel(samp, float2(tc3.x,  tc0.y), 0.f).rgb  * (w3.x  * w0.y) +
        tex.SampleLevel(samp, float2(tc3.x,  tc12.y), 0.f).rgb * (w3.x  * w12.y) +
        tex.SampleLevel(samp, float2(tc3.x,  tc3.y), 0.f).rgb  * (w3.x  * w3.y);

    return result;
}


[RootSignature(ROOT_SIGNATURE)]
[numthreads(16, 16, 1)]
void main(uint3 DTid : SV_DispatchThreadID) 
{
    const int2 offset[8] = { int2(-1, -1), int2(-1,  1),
                              int2( 1, -1), int2( 1,  1),
                              int2( 1,  0), int2( 0, -1),
                              int2( 0,  1), int2(-1,  0), };

    uint2 texDim;
    uint levels;
    gTexColor.GetDimensions(0, texDim.x, texDim.y, levels);
    
    int2 ipos = DTid.xy;

    // Fetch the current pixel color and compute the color bounding box
    // Details here: http://www.gdcvault.com/play/1023521/From-the-Lab-Bench-Real
    // and here: http://cwyman.org/papers/siga16_gazeTrackedFoveatedRendering.pdf
    float3 color = gTexColor.Load(int3(ipos, 0)).rgb;
    color = RGBToYCgCo(color);
    float3 colorAvg = color;
    float3 colorVar = color * color;
    [unroll]
    for (int k = 0; k < 8; k++)
    {
        float3 c = gTexColor.Load(int3(ipos + offset[k], 0)).rgb;
        c = RGBToYCgCo(c);
        colorAvg += c;
        colorVar += c * c;
    }

    float oneOverNine = 1.f / 9.f;
    colorAvg *= oneOverNine;
    colorVar *= oneOverNine;

    float3 sigma = sqrt(max(0.f, colorVar - colorAvg * colorAvg));
    float3 colorMin = colorAvg - gColorBoxSigma * sigma;
    float3 colorMax = colorAvg + gColorBoxSigma * sigma;

    // Find the longest motion vector
    float2 motion = gTexMotionVec.Load(int3(ipos, 0)).xy;
    [unroll]
    for (int a = 0; a < 8; a++)
    {
        float2 m = gTexMotionVec.Load(int3(ipos + offset[a], 0)).rg;
        motion = dot(m, m) > dot(motion, motion) ? m : motion;
    }

    // Use motion vector to fetch previous frame color (history)
    float3 history = bicubicSampleCatmullRom(gTexPrevColor, gSampler, ipos + motion * texDim, texDim);

    history = RGBToYCgCo(history);

    // Anti-flickering, based on Brian Karis talk @Siggraph 2014
    // https://de45xmedrsdbp.cloudfront.net/Resources/files/TemporalAA_small-59732822.pdf
    // Reduce blend factor when history is near clamping
    float distToClamp = min(abs(colorMin.x - history.x), abs(colorMax.x - history.x));
    float alpha = clamp((gAlpha * distToClamp) / (distToClamp + colorMax.x - colorMin.x), 0.f, 1.f);

    history = clamp(history, colorMin, colorMax);
    float3 result = YCgCoToRGB(lerp(history, color, alpha));
    gOutput[ipos] = float4(result, 1.f);
}
