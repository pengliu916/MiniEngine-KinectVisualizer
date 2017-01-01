#include "SensorTexGen.inl"
#include "SensorTexGen.hlsli"
#include "CalibData.inl"

#if COLOR_TEX
RWTexture2D<float4> ColorTex : register(u3);
#endif // COLOR_TEX
#if DEPTH_TEX
RWTexture2D<uint> DepthTex : register(u0);
#endif // DEPTH_TEX
#if INFRARED_TEX
RWTexture2D<float4> InfraredTex : register(u2);
#endif // INFRARED_TEX
#if VISUALIZED_DEPTH_TEX
RWTexture2D<float4> DepthVisualTex : register(u1);
#endif // VISUALIZED_DEPTH_TEX

[numthreads(THREAD_PER_GROUP, 1, 1)]
void main(uint3 Gid : SV_GroupID,
    uint GI : SV_GroupIndex, uint3 GTid : SV_GroupThreadID,
    uint3 DTid : SV_DispatchThreadID)
{
    uint uId = Gid.x * THREAD_PER_GROUP + GI;
    uint2 u2Out_xy = uint2(uId % U2RESO.x, uId / U2RESO.x);
    float4 f4w;
#if UNDISTORTION
    float2 f2New_xy = correctDistortion(f2c, f2f, f2p, f4k, u2Out_xy);
    int2 i2Idx00;
    f4w = GetBilinearWeights(modf(f2New_xy, i2Idx00));
    uId = (uint)i2Idx00.y * U2RESO.x + (uint)i2Idx00.x;
#endif // UNDISTORTION

#if COLOR_TEX
    ColorTex[u2Out_xy] = GetColor(u2Out_xy, uId, f4w);
#endif // COLOR_TEX

#if VISUALIZED_DEPTH_TEX
    DepthVisualTex[u2Out_xy] =
        (GetDepth(u2Out_xy, uId, f4w).xxxx % 255) / 255.f;
#endif // VISUALIZED_DEPTH_TEX

#if INFRARED_TEX
    InfraredTex[u2Out_xy] = GetInfrared(u2Out_xy, uId, f4w);
#endif // INFRARED_TEX

#if DEPTH_TEX
    DepthTex[u2Out_xy] = GetDepth(u2Out_xy, uId, f4w);
#endif // COLOR_TEX
} 