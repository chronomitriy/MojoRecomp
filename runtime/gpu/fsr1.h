#pragma once

// AMD FidelityFX Super Resolution 1 (FSR 1): EASU upscaling and RCAS
// sharpening for the presenter's final output.
//
// The constant setup and the HLSL below are ports of the 32-bit paths of
// ffx_fsr1.h v1.20210629 and the helpers they use from ffx_a.h, from
// https://github.com/GPUOpen-Effects/FidelityFX-FSR (commit a21ffb8f):
//
//   Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
//
//   Permission is hereby granted, free of charge, to any person obtaining a copy
//   of this software and associated documentation files (the "Software"), to deal
//   in the Software without restriction, including without limitation the rights
//   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//   copies of the Software, and to permit persons to whom the Software is
//   furnished to do so, subject to the following conditions:
//
//   The above copyright notice and this permission notice shall be included in
//   all copies or substantial portions of the Software.
//
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
//   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
//   THE SOFTWARE.
//
// The same notice ships as launcher/resources/licenses/AMD-FidelityFX-FSR1-LICENSE.txt.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace mojorecomp::gpu::fsr1 {

struct EasuConstants
{
    std::array<uint32_t, 4> con0{};
    std::array<uint32_t, 4> con1{};
    std::array<uint32_t, 4> con2{};
    std::array<uint32_t, 4> con3{};
};

struct RcasConstants
{
    std::array<uint32_t, 4> con{};
};

// Push-constant blocks of the two pixel shaders below.
struct EasuPush
{
    std::array<uint32_t, 4> con0{};
    std::array<uint32_t, 4> con1{};
    std::array<uint32_t, 4> con2{};
    std::array<uint32_t, 4> con3{};
};
static_assert(sizeof(EasuPush) == 64);

struct RcasPush
{
    std::array<uint32_t, 4> con{};
    // Top-left of the output rectangle in the render target.
    int32_t offset[2]{};
    // Last valid texel of the source image.
    uint32_t maxTexel[2]{};
};
static_assert(sizeof(RcasPush) == 32);

inline uint32_t FloatBits(float value)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

inline float BitsFloat(uint32_t bits)
{
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// FsrEasuCon: the input viewport is the whole input image here.
inline EasuConstants ComputeEasuConstants(float inputViewportX, float inputViewportY,
                                          float inputSizeX, float inputSizeY,
                                          float outputSizeX, float outputSizeY)
{
    EasuConstants c{};
    // Output integer position to a pixel position in the viewport.
    c.con0[0] = FloatBits(inputViewportX * (1.0f / outputSizeX));
    c.con0[1] = FloatBits(inputViewportY * (1.0f / outputSizeY));
    c.con0[2] = FloatBits(0.5f * inputViewportX * (1.0f / outputSizeX) - 0.5f);
    c.con0[3] = FloatBits(0.5f * inputViewportY * (1.0f / outputSizeY) - 0.5f);
    // Viewport pixel position to normalized image space (upper-left of 'F').
    c.con1[0] = FloatBits(1.0f / inputSizeX);
    c.con1[1] = FloatBits(1.0f / inputSizeY);
    // Centres of the gathers, first from the upper-left of 'F', then from (0).
    c.con1[2] = FloatBits(1.0f * (1.0f / inputSizeX));
    c.con1[3] = FloatBits(-1.0f * (1.0f / inputSizeY));
    c.con2[0] = FloatBits(-1.0f * (1.0f / inputSizeX));
    c.con2[1] = FloatBits(2.0f * (1.0f / inputSizeY));
    c.con2[2] = FloatBits(1.0f * (1.0f / inputSizeX));
    c.con2[3] = FloatBits(2.0f * (1.0f / inputSizeY));
    c.con3[0] = FloatBits(0.0f * (1.0f / inputSizeX));
    c.con3[1] = FloatBits(4.0f * (1.0f / inputSizeY));
    c.con3[2] = 0;
    c.con3[3] = 0;
    return c;
}

// IEEE half of a float, round to nearest even (ffx_a.h's AU1_AH1_AF1 rounds the
// same way for normal values). Only con[1] uses it, for 16-bit RCAS variants.
inline uint16_t FloatToHalf(float value)
{
    const uint32_t bits = FloatBits(value);
    const uint32_t sign = (bits >> 16) & 0x8000u;
    const int32_t exponent = int32_t((bits >> 23) & 0xFFu) - 127 + 15;
    uint32_t mantissa = bits & 0x7FFFFFu;
    if (((bits >> 23) & 0xFFu) == 0xFFu)
        return uint16_t(sign | 0x7C00u | (mantissa ? 0x200u : 0u));
    if (exponent >= 31)
        return uint16_t(sign | 0x7C00u);
    if (exponent <= 0)
    {
        if (exponent < -10)
            return uint16_t(sign);
        mantissa |= 0x800000u;
        const uint32_t shift = uint32_t(14 - exponent);
        uint32_t half = mantissa >> shift;
        const uint32_t rest = mantissa & ((1u << shift) - 1u);
        const uint32_t halfway = 1u << (shift - 1u);
        if (rest > halfway || (rest == halfway && (half & 1u)))
            ++half;
        return uint16_t(sign | half);
    }
    uint32_t half = (uint32_t(exponent) << 10) | (mantissa >> 13);
    const uint32_t rest = mantissa & 0x1FFFu;
    if (rest > 0x1000u || (rest == 0x1000u && (half & 1u)))
        ++half;
    return uint16_t(sign | half);
}

// FsrRcasCon. `stops` is AMD's scale: 0 is the maximum sharpness, each further
// stop halves it.
inline RcasConstants ComputeRcasConstants(float stops)
{
    RcasConstants c{};
    const float sharpness = std::exp2(-stops);
    c.con[0] = FloatBits(sharpness);
    const uint32_t half = FloatToHalf(sharpness);
    c.con[1] = half | (half << 16);
    c.con[2] = 0;
    c.con[3] = 0;
    return c;
}

// The runtime setting is 0..1, higher is sharper: 1 is AMD's maximum (0 stops),
// 0 is two stops softer.
inline float SharpnessToStops(float sharpness)
{
    const float clamped = std::clamp(std::isfinite(sharpness) ? sharpness : 0.0f, 0.0f, 1.0f);
    return 2.0f * (1.0f - clamped);
}

// EASU only scales up: each output dimension must be at least the input's.
inline bool EasuApplies(uint32_t inputWidth, uint32_t inputHeight, uint32_t outputWidth,
                        uint32_t outputHeight)
{
    return inputWidth && inputHeight && outputWidth >= inputWidth &&
           outputHeight >= inputHeight &&
           (outputWidth > inputWidth || outputHeight > inputHeight);
}

// Full-screen triangle; the pixel shaders below address texels by SV_Position.
inline constexpr const char* kFullscreenVertexShaderHlsl = R"(
float4 main(uint id : SV_VertexID) : SV_Position
{
    const float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
)";

// EASU: ffx_fsr1.h FsrEasuF (FSR_EASU_F), one output pixel per fragment. The
// render target is exactly the output size, so SV_Position is the output pixel.
inline constexpr const char* kEasuPixelShaderHlsl = R"(
[[vk::binding(0, 0)]] Texture2D<float4> g_Source;
[[vk::binding(1, 0)]] SamplerState g_Sampler;
struct EasuPush
{
    uint4 con0;
    uint4 con1;
    uint4 con2;
    uint4 con3;
};
[[vk::push_constant]] ConstantBuffer<EasuPush> g_Push;

float APrxLoRcpF1(float a) { return asfloat(0x7ef07ebbu - asuint(a)); }
float APrxLoRsqF1(float a) { return asfloat(0x5f347d74u - (asuint(a) >> 1u)); }
float3 AMin3F3(float3 x, float3 y, float3 z) { return min(x, min(y, z)); }
float3 AMax3F3(float3 x, float3 y, float3 z) { return max(x, max(y, z)); }

float4 FsrEasuRF(float2 p) { return g_Source.GatherRed(g_Sampler, p); }
float4 FsrEasuGF(float2 p) { return g_Source.GatherGreen(g_Sampler, p); }
float4 FsrEasuBF(float2 p) { return g_Source.GatherBlue(g_Sampler, p); }

void FsrEasuTapF(inout float3 aC, inout float aW, float2 off, float2 dir, float2 len,
                 float lob, float clp, float3 c)
{
    float2 v;
    v.x = (off.x * ( dir.x)) + (off.y * dir.y);
    v.y = (off.x * (-dir.y)) + (off.y * dir.x);
    v *= len;
    float d2 = v.x * v.x + v.y * v.y;
    d2 = min(d2, clp);
    float wB = (2.0 / 5.0) * d2 + (-1.0);
    float wA = lob * d2 + (-1.0);
    wB *= wB;
    wA *= wA;
    wB = (25.0 / 16.0) * wB + (-(25.0 / 16.0 - 1.0));
    const float w = wB * wA;
    aC += c * w;
    aW += w;
}

void FsrEasuSetF(inout float2 dir, inout float len, float2 pp,
                 bool biS, bool biT, bool biU, bool biV,
                 float lA, float lB, float lC, float lD, float lE)
{
    float w = 0.0;
    if (biS) w = (1.0 - pp.x) * (1.0 - pp.y);
    if (biT) w =        pp.x  * (1.0 - pp.y);
    if (biU) w = (1.0 - pp.x) *        pp.y ;
    if (biV) w =        pp.x  *        pp.y ;
    const float dc = lD - lC;
    const float cb = lC - lB;
    float lenX = max(abs(dc), abs(cb));
    lenX = APrxLoRcpF1(lenX);
    const float dirX = lD - lB;
    dir.x += dirX * w;
    lenX = saturate(abs(dirX) * lenX);
    lenX *= lenX;
    len += lenX * w;
    const float ec = lE - lC;
    const float ca = lC - lA;
    float lenY = max(abs(ec), abs(ca));
    lenY = APrxLoRcpF1(lenY);
    const float dirY = lE - lA;
    dir.y += dirY * w;
    lenY = saturate(abs(dirY) * lenY);
    lenY *= lenY;
    len += lenY * w;
}

float3 FsrEasuF(uint2 ip, uint4 con0, uint4 con1, uint4 con2, uint4 con3)
{
    float2 pp = float2(ip) * asfloat(con0.xy) + asfloat(con0.zw);
    const float2 fp = floor(pp);
    pp -= fp;
    const float2 p0 = fp * asfloat(con1.xy) + asfloat(con1.zw);
    const float2 p1 = p0 + asfloat(con2.xy);
    const float2 p2 = p0 + asfloat(con2.zw);
    const float2 p3 = p0 + asfloat(con3.xy);
    const float4 bczzR = FsrEasuRF(p0);
    const float4 bczzG = FsrEasuGF(p0);
    const float4 bczzB = FsrEasuBF(p0);
    const float4 ijfeR = FsrEasuRF(p1);
    const float4 ijfeG = FsrEasuGF(p1);
    const float4 ijfeB = FsrEasuBF(p1);
    const float4 klhgR = FsrEasuRF(p2);
    const float4 klhgG = FsrEasuGF(p2);
    const float4 klhgB = FsrEasuBF(p2);
    const float4 zzonR = FsrEasuRF(p3);
    const float4 zzonG = FsrEasuGF(p3);
    const float4 zzonB = FsrEasuBF(p3);
    const float4 bczzL = bczzB * 0.5 + (bczzR * 0.5 + bczzG);
    const float4 ijfeL = ijfeB * 0.5 + (ijfeR * 0.5 + ijfeG);
    const float4 klhgL = klhgB * 0.5 + (klhgR * 0.5 + klhgG);
    const float4 zzonL = zzonB * 0.5 + (zzonR * 0.5 + zzonG);
    const float bL = bczzL.x;
    const float cL = bczzL.y;
    const float iL = ijfeL.x;
    const float jL = ijfeL.y;
    const float fL = ijfeL.z;
    const float eL = ijfeL.w;
    const float kL = klhgL.x;
    const float lL = klhgL.y;
    const float hL = klhgL.z;
    const float gL = klhgL.w;
    const float oL = zzonL.z;
    const float nL = zzonL.w;
    float2 dir = 0.0;
    float len = 0.0;
    FsrEasuSetF(dir, len, pp, true,  false, false, false, bL, eL, fL, gL, jL);
    FsrEasuSetF(dir, len, pp, false, true,  false, false, cL, fL, gL, hL, kL);
    FsrEasuSetF(dir, len, pp, false, false, true,  false, fL, iL, jL, kL, nL);
    FsrEasuSetF(dir, len, pp, false, false, false, true,  gL, jL, kL, lL, oL);
    const float2 dir2 = dir * dir;
    float dirR = dir2.x + dir2.y;
    const bool zro = dirR < (1.0 / 32768.0);
    dirR = APrxLoRsqF1(dirR);
    dirR = zro ? 1.0 : dirR;
    dir.x = zro ? 1.0 : dir.x;
    dir *= dirR;
    len = len * 0.5;
    len *= len;
    const float stretch = (dir.x * dir.x + dir.y * dir.y) * APrxLoRcpF1(max(abs(dir.x), abs(dir.y)));
    const float2 len2 = float2(1.0 + (stretch - 1.0) * len, 1.0 + (-0.5) * len);
    const float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    const float clp = APrxLoRcpF1(lob);
    const float3 min4 = min(AMin3F3(float3(ijfeR.z, ijfeG.z, ijfeB.z), float3(klhgR.w, klhgG.w, klhgB.w),
                                    float3(ijfeR.y, ijfeG.y, ijfeB.y)),
                            float3(klhgR.x, klhgG.x, klhgB.x));
    const float3 max4 = max(AMax3F3(float3(ijfeR.z, ijfeG.z, ijfeB.z), float3(klhgR.w, klhgG.w, klhgB.w),
                                    float3(ijfeR.y, ijfeG.y, ijfeB.y)),
                            float3(klhgR.x, klhgG.x, klhgB.x));
    float3 aC = 0.0;
    float aW = 0.0;
    FsrEasuTapF(aC, aW, float2( 0.0, -1.0) - pp, dir, len2, lob, clp, float3(bczzR.x, bczzG.x, bczzB.x));
    FsrEasuTapF(aC, aW, float2( 1.0, -1.0) - pp, dir, len2, lob, clp, float3(bczzR.y, bczzG.y, bczzB.y));
    FsrEasuTapF(aC, aW, float2(-1.0,  1.0) - pp, dir, len2, lob, clp, float3(ijfeR.x, ijfeG.x, ijfeB.x));
    FsrEasuTapF(aC, aW, float2( 0.0,  1.0) - pp, dir, len2, lob, clp, float3(ijfeR.y, ijfeG.y, ijfeB.y));
    FsrEasuTapF(aC, aW, float2( 0.0,  0.0) - pp, dir, len2, lob, clp, float3(ijfeR.z, ijfeG.z, ijfeB.z));
    FsrEasuTapF(aC, aW, float2(-1.0,  0.0) - pp, dir, len2, lob, clp, float3(ijfeR.w, ijfeG.w, ijfeB.w));
    FsrEasuTapF(aC, aW, float2( 1.0,  1.0) - pp, dir, len2, lob, clp, float3(klhgR.x, klhgG.x, klhgB.x));
    FsrEasuTapF(aC, aW, float2( 2.0,  1.0) - pp, dir, len2, lob, clp, float3(klhgR.y, klhgG.y, klhgB.y));
    FsrEasuTapF(aC, aW, float2( 2.0,  0.0) - pp, dir, len2, lob, clp, float3(klhgR.z, klhgG.z, klhgB.z));
    FsrEasuTapF(aC, aW, float2( 1.0,  0.0) - pp, dir, len2, lob, clp, float3(klhgR.w, klhgG.w, klhgB.w));
    FsrEasuTapF(aC, aW, float2( 1.0,  2.0) - pp, dir, len2, lob, clp, float3(zzonR.z, zzonG.z, zzonB.z));
    FsrEasuTapF(aC, aW, float2( 0.0,  2.0) - pp, dir, len2, lob, clp, float3(zzonR.w, zzonG.w, zzonB.w));
    return min(max4, max(min4, aC * rcp(aW)));
}

float4 main(float4 position : SV_Position) : SV_Target0
{
    return float4(FsrEasuF(uint2(position.xy), g_Push.con0, g_Push.con1, g_Push.con2,
                           g_Push.con3), 1.0);
}
)";

// RCAS: ffx_fsr1.h FsrRcasF (FSR_RCAS_F, no denoise, no alpha passthrough).
// Texel loads are clamped to the image, the edge behaviour of a clamping sampler.
inline constexpr const char* kRcasPixelShaderHlsl = R"(
[[vk::binding(0, 0)]] Texture2D<float4> g_Source;
struct RcasPush
{
    uint4 con;
    int2 offset;
    uint2 maxTexel;
};
[[vk::push_constant]] ConstantBuffer<RcasPush> g_Push;

#define FSR_RCAS_LIMIT (0.25 - (1.0 / 16.0))

float APrxMedRcpF1(float a)
{
    const float b = asfloat(0x7ef19fffu - asuint(a));
    return b * (-b * a + 2.0);
}
float AMin3F1(float x, float y, float z) { return min(x, min(y, z)); }
float AMax3F1(float x, float y, float z) { return max(x, max(y, z)); }

float4 FsrRcasLoadF(int2 p)
{
    const int2 clamped = clamp(p, int2(0, 0), int2(g_Push.maxTexel));
    return g_Source.Load(int3(clamped, 0));
}

float3 FsrRcasF(uint2 ip, uint4 con)
{
    const int2 sp = int2(ip);
    const float3 b = FsrRcasLoadF(sp + int2( 0, -1)).rgb;
    const float3 d = FsrRcasLoadF(sp + int2(-1,  0)).rgb;
    const float3 e = FsrRcasLoadF(sp).rgb;
    const float3 f = FsrRcasLoadF(sp + int2( 1,  0)).rgb;
    const float3 h = FsrRcasLoadF(sp + int2( 0,  1)).rgb;
    const float bR = b.r, bG = b.g, bB = b.b;
    const float dR = d.r, dG = d.g, dB = d.b;
    const float eR = e.r, eG = e.g, eB = e.b;
    const float fR = f.r, fG = f.g, fB = f.b;
    const float hR = h.r, hG = h.g, hB = h.b;
    const float mn4R = min(AMin3F1(bR, dR, fR), hR);
    const float mn4G = min(AMin3F1(bG, dG, fG), hG);
    const float mn4B = min(AMin3F1(bB, dB, fB), hB);
    const float mx4R = max(AMax3F1(bR, dR, fR), hR);
    const float mx4G = max(AMax3F1(bG, dG, fG), hG);
    const float mx4B = max(AMax3F1(bB, dB, fB), hB);
    const float2 peakC = float2(1.0, -1.0 * 4.0);
    const float hitMinR = min(mn4R, eR) * rcp(4.0 * mx4R);
    const float hitMinG = min(mn4G, eG) * rcp(4.0 * mx4G);
    const float hitMinB = min(mn4B, eB) * rcp(4.0 * mx4B);
    const float hitMaxR = (peakC.x - max(mx4R, eR)) * rcp(4.0 * mn4R + peakC.y);
    const float hitMaxG = (peakC.x - max(mx4G, eG)) * rcp(4.0 * mn4G + peakC.y);
    const float hitMaxB = (peakC.x - max(mx4B, eB)) * rcp(4.0 * mn4B + peakC.y);
    const float lobeR = max(-hitMinR, hitMaxR);
    const float lobeG = max(-hitMinG, hitMaxG);
    const float lobeB = max(-hitMinB, hitMaxB);
    const float lobe = max(-FSR_RCAS_LIMIT, min(AMax3F1(lobeR, lobeG, lobeB), 0.0)) * asfloat(con.x);
    const float rcpL = APrxMedRcpF1(4.0 * lobe + 1.0);
    return float3((lobe * bR + lobe * dR + lobe * hR + lobe * fR + eR) * rcpL,
                  (lobe * bG + lobe * dG + lobe * hG + lobe * fG + eG) * rcpL,
                  (lobe * bB + lobe * dB + lobe * hB + lobe * fB + eB) * rcpL);
}

float4 main(float4 position : SV_Position) : SV_Target0
{
    const int2 pixel = int2(position.xy) - g_Push.offset;
    return float4(FsrRcasF(uint2(pixel), g_Push.con), 1.0);
}
)";

} // namespace mojorecomp::gpu::fsr1
