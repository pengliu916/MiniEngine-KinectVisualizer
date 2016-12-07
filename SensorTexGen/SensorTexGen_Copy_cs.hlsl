#include "SensorTexGen.inl"
#include "SensorTexGen.hlsli"
#include "CalibData.inl"

#if COLOR_TEX
Buffer<uint4> ColBuffer : register(t2);
RWTexture2D<float4> ColorTex : register(u3);
#else
Buffer<uint> DepBuffer : register(t0);
RWTexture2D<uint> DepthTex : register(u0);
#if INFRARED_TEX
Buffer<uint> InfBuffer : register(t1);
RWTexture2D<float4> InfraredTex : register(u2);
#endif // INFRARED_TEX
#if VISUALIZED_DEPTH_TEX
RWTexture2D<float4> DepthVisualTex : register(u1);
#endif // VISUALIZED_DEPTH_TEX
#endif

uint GetFakedDepth(uint2 u2uv)
{
    float2 f2ab = (u2uv - f2c) / f2f;
    float fA = dot(f2ab, f2ab) + 1.f;
    float fB = -2.f * (dot(f2ab, f4S.xy) + f4S.z);
    float fC = dot(f4S.xyz, f4S.xyz) - f4S.w * f4S.w;
    float fB2M4AC = fB * fB - 4.f * fA * fC;
    if (fB2M4AC < 0.f) {
        return fBgDist * 1000.f;
    } else {
        float fz = min((-fB - sqrt(fB2M4AC)) / (2.f * fA), fBgDist);
        return fz * 1000;
    }
}

[numthreads(THREAD_PER_GROUP, 1, 1)]
void main(uint3 Gid : SV_GroupID, 
    uint GI : SV_GroupIndex, uint3 GTid : SV_GroupThreadID, 
    uint3 DTid : SV_DispatchThreadID)
{
    uint uId = Gid.x * THREAD_PER_GROUP + GI;
    uint2 u2Out_xy = uint2(uId % F2RESO.x, uId / F2RESO.x);

#if UNDISTORTION
    float2 f2New_xy = 
        correctDistortion(f2c, f2f, f2p, f4k, u2Out_xy);
    uId = (uint)f2New_xy.y * F2RESO.x + (uint)f2New_xy.x;
#endif // UNDISTORTION
#if COLOR_TEX
    ColorTex[u2Out_xy] = ColBuffer[uId] / 255.f;
#else
#if VISUALIZED_DEPTH_TEX
    //DepthVisualTex[u2Out_xy] = (GetFakedDepth(u2Out_xy).xxxx % 255) / 255.f;
    DepthVisualTex[u2Out_xy] = (DepBuffer[uId].xxxx % 255) / 255.f;
#endif // VISUALIZED_DEPTH_TEX
#if INFRARED_TEX
    InfraredTex[u2Out_xy] = pow(InfBuffer[uId].xxxx / 65535.f, 0.32f);
#endif // INFRARED_TEX
    //DepthTex[u2Out_xy] = fBgDist * 1000.f;
    //DepthTex[u2Out_xy] = GetFakedDepth(u2Out_xy);
    DepthTex[u2Out_xy] = DepBuffer[uId];
#endif // COLOR_TEX
}