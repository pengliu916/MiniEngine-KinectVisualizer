float4 GetBilinearWeights(float2 f2uv)
{
    float4 f4w;
    float2 f2uv_1 = 1.f - f2uv;
    f4w.x = f2uv_1.x * f2uv_1.y;
    f4w.y = f2uv.x * f2uv_1.y;
    f4w.z = f2uv.y * f2uv_1.x;
    f4w.w = f2uv.x * f2uv.y;
    return f4w;
}

float2 correctDistortion(float2 c, float2 f, float2 p, float4 k, uint2 idx)
{
    // For opencv r is distance from 0,0 to X/Z,Y/Z 
    float2 xy = (idx - c) / f;
    float r2 = dot(xy, xy);
    float r4 = r2 * r2;
    float r6 = r2 * r4;
    // For radial and tangential distortion correction
    float2 new_xy =
        xy * (1.f + k.x * r2 + k.y * r4 + k.z * r6) +
        2.f * p * xy.x * xy.y + p.yx * (r2 + 2.f * xy * xy);
    return new_xy * f + c;
}

#if COLOR_TEX
static const float2 f2c = COLOR_C;
static const float2 f2f = COLOR_F;
static const float2 f2p = COLOR_P;
static const float4 f4k = COLOR_K;
#define U2RESO u2ColorReso
Buffer<uint4> ColBuffer : register(t2);

float4 GetColor(uint2 u2Out_xy, uint uId, float4 f4w)
{
#   if UNDISTORTION
    float4 f400 = ColBuffer[uId];
    float4 f410 = ColBuffer[uId + 1];
    float4 f401 = ColBuffer[uId + U2RESO.x];
    float4 f411 = ColBuffer[uId + U2RESO.x + 1];
    return (f400 * f4w.x + f410 * f4w.y + f401 * f4w.z + f411 * f4w.w) / 255.f;
#   else
    return ColBuffer[uId] / 255.f;
#   endif // UNDISTORTION
}
#endif // COLOR_TEX

#if DEPTH_TEX || INFRARED_TEX || VISUALIZED_DEPTH_TEX
static const float2 f2c = DEPTH_C;
static const float2 f2f = DEPTH_F;
static const float2 f2p = DEPTH_P;
static const float4 f4k = DEPTH_K;
#define U2RESO u2DepthInfraredReso
Buffer<uint> DepBuffer : register(t0);

uint GetFakedDepth(uint2 u2uv)
{
    float fResult = fBgDist;
    float2 f2ab = (u2uv - f2c) / f2f;
    float fA = dot(f2ab, f2ab) + 1.f;
    float fB = -2.f * (dot(f2ab, f4S.xy) + f4S.z);
    float fC = dot(f4S.xyz, f4S.xyz) - f4S.w * f4S.w;
    float fB2M4AC = fB * fB - 4.f * fA * fC;
    if (fB2M4AC >= 0.f) {
        fResult = min((-fB - sqrt(fB2M4AC)) / (2.f * fA), fBgDist);
    }
    if (any(u2uv == uint2(12, 12)) || any(u2uv == uint2(500, 412)) ||
        any(u2uv == uint2(256, 212))) {
        fResult += .2f;
    }
    return fResult * 1000.f;
}

uint GetDepth(uint2 u2Out_xy, uint uId, float4 f4w)
{
#   if FAKEDDEPTH
    return GetFakedDepth(u2Out_xy);
#   else
#       if UNDISTORTION
    uint4 u4;
    u4.x = DepBuffer[uId];
    u4.y = DepBuffer[uId + 1];
    u4.z = DepBuffer[uId + U2RESO.x];
    u4.w = DepBuffer[uId + U2RESO.x + 1];
    float4 f4 = (u4 > 200 && u4 < 10000);
    return (uint)(dot(u4 * f4, f4w) / dot(f4, f4w) + 0.5f);
#       else
    return DepBuffer[uId];
#       endif // UNDISTORTION
#   endif // FAKEDDEPTH
}
#   if INFRARED_TEX
Buffer<uint> InfBuffer : register(t1);
float GetInfrared(uint2 u2Out_xy, uint uId, float4 f4w)
{
#       if UNDISTORTION
    uint4 u4;
    u4.x = InfBuffer[uId];
    u4.y = InfBuffer[uId + 1];
    u4.z = InfBuffer[uId + U2RESO.x];
    u4.w = InfBuffer[uId + U2RESO.x + 1];
#pragma warning(push)
    // dot(u4, f4w) guarantee to return positive so suppress the warning
#pragma warning(disable: 3571)
    return pow(dot(u4, f4w) / 65535.f, 0.32f);
#pragma warning(pop)
#       else
    return pow(InfBuffer[uId] / 65535.f, 0.32f);
#       endif // UNDISTORTION
}
#   endif // INFRARED_TEX
#endif // DEPTH_TEX || INFRARED_TEX || VISUALIZED_DEPTH_TEX