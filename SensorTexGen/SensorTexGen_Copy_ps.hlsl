#include "SensorTexGen.inl"
#include "SensorTexGen.hlsli"
#include "CalibData.inl"

struct PSOutput {
#if COLOR_TEX
    float4 f4ColorOut : SV_Target0;
#endif // COLOR_TEX
#if DEPTH_TEX
    float fDepthOut : SV_Target0;
#endif // DEPTH_TEX
#if VISUALIZED_DEPTH_TEX
    float4 f4DepVisaulOut : SV_Target1;
#endif // VISUALIZED_DEPTH_TEX
#if INFRARED_TEX
    float4 f4InfraredOut : SV_Target2;
#endif // INFRARED_TEX
};

PSOutput main(
    float4 f4Pos : SV_Position, float2 f2UV : TEXCOORD0)
{
    PSOutput output;
    uint2 u2Out_xy = (uint2)(f2UV * U2RESO);
    float4 f4w;
#if UNDISTORTION
    float2 f2New_xy = correctDistortion(f2c, f2f, f2p, f4k, u2Out_xy);
    int2 i2Idx00;
    f4w = GetBilinearWeights(modf(f2New_xy, i2Idx00));
    uint uId = (uint)i2Idx00.y * U2RESO.x + (uint)i2Idx00.x;
#else
    uint uId = u2Out_xy.y * U2RESO.x + u2Out_xy.x;
#endif // UNDISTORTION

#if COLOR_TEX
    output.f4ColorOut = GetColor(u2Out_xy, uId, f4w);
#endif // COLOR_TEX

#if VISUALIZED_DEPTH_TEX
    output.f4DepVisaulOut =
        ((uint)(GetNormDepth(u2Out_xy, uId, f4w) * 10000) % 255) / 255.f;
#endif // VISUALIZED_DEPTH_TEX

#if INFRARED_TEX
    output.f4InfraredOut = GetInfrared(u2Out_xy, uId, f4w);
#endif // INFRARED_TEX

#if DEPTH_TEX
    output.fDepthOut = GetNormDepth(u2Out_xy, uId, f4w);
#endif // DEPTH_TEX
    return output;
}