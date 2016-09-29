#include "SensorTexGen.inl"
#include "SensorTexGen.hlsli"
#include "CalibData.inl"

#if COLOR_TEX
Buffer<uint4> ColBuffer : register(t2);
#else
Buffer<uint> DepBuffer : register(t0);
#if INFRARED_TEX
Buffer<uint> InfBuffer : register(t1);
#endif // INFRARED_TEX
#endif

struct PSOutput {
#if COLOR_TEX
    float4 f4ColorOut : SV_Target0;
#else
    uint uDepthOut : SV_Target0;
#if VISUALIZED_DEPTH_TEX
    float4 f4DepVisaulOut : SV_Target1;
#endif // VISUALIZED_DEPTH_TEX
#if INFRARED_TEX
    float4 f4InfraredOut : SV_Target2;
#endif // INFRARED_TEX
#endif // COLOR_TEX
};

PSOutput main(
    float4 f4Pos : SV_Position, float2 f2UV : TEXCOORD0)
{
    PSOutput output;
    uint2 u2Idx = (uint2)(f2UV * F2RESO);
#if UNDISTORTION
    float2 f2New_xy = correctDistortion(f2c, f2f, f2p, f4k, u2Idx);
    uint uId = (uint)f2New_xy.y * F2RESO.x + (uint)f2New_xy.x;
#else
    uint uId = u2Idx.y * F2RESO.x + u2Idx.x;
#endif // UNDISTORTION
#if COLOR_TEX
    output.f4ColorOut = ColBuffer[uId] / 255.f;
#else
#if VISUALIZED_DEPTH_TEX
    output.f4DepVisaulOut = (DepBuffer[uId] % 255) / 255.f;
#endif // VISUALIZED_DEPTH_TEX
#if INFRARED_TEX
    output.f4InfraredOut = pow((InfBuffer[uId]) / 65535.f, 0.32f);
#endif // INFRARED_TEX
    output.uDepthOut = DepBuffer[uId];
#endif // COLOR_TEX
    return output;
}