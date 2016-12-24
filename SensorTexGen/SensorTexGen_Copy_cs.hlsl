#include "SensorTexGen.inl"
#include "SensorTexGen.hlsli"
#include "CalibData.inl"

#if COLOR_TEX
RWTexture2D<float4> ColorTex : register(u3);
#else
RWTexture2D<uint> DepthTex : register(u0);
#if INFRARED_TEX
RWTexture2D<float4> InfraredTex : register(u2);
#endif // INFRARED_TEX
#if VISUALIZED_DEPTH_TEX
RWTexture2D<float4> DepthVisualTex : register(u1);
#endif // VISUALIZED_DEPTH_TEX
#endif

[numthreads(THREAD_PER_GROUP, 1, 1)]
void main(uint3 Gid : SV_GroupID,
    uint GI : SV_GroupIndex, uint3 GTid : SV_GroupThreadID,
    uint3 DTid : SV_DispatchThreadID)
{
    uint uId = Gid.x * THREAD_PER_GROUP + GI;
    uint2 u2Out_xy = uint2(uId % F2RESO.x, uId / F2RESO.x);

#if UNDISTORTION
    float2 f2New_xy = correctDistortion(f2c, f2f, f2p, f4k, u2Out_xy);
    uId = (uint)f2New_xy.y * F2RESO.x + (uint)f2New_xy.x;
#endif // UNDISTORTION
#if COLOR_TEX
    ColorTex[u2Out_xy] = ColBuffer[uId] / 255.f;
#else
#if VISUALIZED_DEPTH_TEX
#if FAKEDEPTH
    DepthVisualTex[u2Out_xy] = (GetFakedDepth(u2Out_xy).xxxx % 255) / 255.f;
#else
    DepthVisualTex[u2Out_xy] = (DepBuffer[uId].xxxx % 255) / 255.f;
#endif // FAKEDEPTH
#endif // VISUALIZED_DEPTH_TEX
#if INFRARED_TEX
    InfraredTex[u2Out_xy] = pow(InfBuffer[uId].xxxx / 65535.f, 0.32f);
#endif // INFRARED_TEX
#if FAKEDEPTH
    DepthTex[u2Out_xy] = GetFakedDepth(u2Out_xy);
#else
    DepthTex[u2Out_xy] = DepBuffer[uId];
#endif // FAKEDEPTH
#endif // COLOR_TEX
}