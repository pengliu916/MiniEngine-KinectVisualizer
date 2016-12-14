#if COLOR_TEX
static const float2 f2c = COLOR_C;
static const float2 f2f = COLOR_F;
static const float2 f2p = COLOR_P;
static const float4 f4k = COLOR_K;
#define F2RESO f2ColorReso
#else
static const float2 f2c = DEPTH_C;
static const float2 f2f = DEPTH_F;
static const float2 f2p = DEPTH_P;
static const float4 f4k = DEPTH_K;
#define F2RESO f2DepthInfraredReso
#endif // COLOR_TEX

#if COLOR_TEX
Buffer<uint4> ColBuffer : register(t2);
#else
Buffer<uint> DepBuffer : register(t0);
#if INFRARED_TEX
Buffer<uint> InfBuffer : register(t1);
#endif // INFRARED_TEX
#endif

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
    // Add 0.5f for correcting float to int truncation
    return new_xy * f + c + 0.5f;
}

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