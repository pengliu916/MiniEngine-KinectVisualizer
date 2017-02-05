#include "NormalGenerator.inl"
#include "CalibData.inl"
//==============================================================================
// Note: the normal is calculated in a way that truly represent normal for the 
//       position in the 'center' of four neighboring points.
//==============================================================================
Texture2D<float> tex_srvNormDepth : register(t0);
RWTexture2D<float4> tex_uavNormal : register(u0);
// Confidence Texture:
// .r: related to dot(surfNor, -viewDir)
// .g: related to 1.f / dot(idx.xy, idx.xy)
// .b: related to 1.f / depth
// .a: overall confidence
#if CONFIDENCE_OUT
RWTexture2D<float4> tex_uavConfidence : register(u1);
#if NO_TYPED_LOAD
Texture2D<float4> tex_srvConfidence : register(t1);
#define CONFIDENCE_TEX tex_srvConfidence
#else
#define CONFIDENCE_TEX tex_uavConfidence
#endif // NO_TYPED_LOAD
#endif // CONFIDENCE_OUT
SamplerState samp_linear : register(s0);

cbuffer Reso : register (b0)
{
    float2 f2InvReso;
}

// Compiler falsely emit x4000 if I do early out return
#pragma warning(disable: 4000)

bool ValidSample(uint2 u2uv)
{
#if CONFIDENCE_OUT
    float4 f4Confidences = {
        CONFIDENCE_TEX[u2uv].a,
        CONFIDENCE_TEX[u2uv + uint2(1, 0)].a,
        CONFIDENCE_TEX[u2uv + uint2(0, 1)].a,
        CONFIDENCE_TEX[u2uv + 1].a};
    if (any(f4Confidences < 0.05f)) {
        return false;
    }
#endif // CONFIDENCE_OUT
    return true;
}

[numthreads(8, 8, 1)]
void main(uint3 u3DTid : SV_DispatchThreadID)
{
    if (!ValidSample(u3DTid.xy)) {
        tex_uavNormal[u3DTid.xy] = 0;
#if CONFIDENCE_OUT
        tex_uavConfidence[u3DTid.xy] = 0.f;
#endif
        return;
    }
    float4 f4zs =
        tex_srvNormDepth.Gather(samp_linear, (u3DTid.xy + .5f) * f2InvReso);
    float fMax = max(f4zs.w, max(f4zs.z, max(f4zs.x, f4zs.y)));
    float fMin = min(f4zs.w, min(f4zs.z, min(f4zs.x, f4zs.y)));
    if (fMin == 0.f || fMax - fMin > 0.1f * fDistThreshold) {
        tex_uavNormal[u3DTid.xy] = 0;
#if CONFIDENCE_OUT
        tex_uavConfidence[u3DTid.xy] = 0.f;
#endif
        return;
    }
    f4zs *= -10.f;
    // Gather order is counter_clockwise from LB
    float4 f4u = float4(u3DTid.x, u3DTid.xx + 1.f, u3DTid.x);
    float4 f4v = float4(u3DTid.yy + 1.f, u3DTid.yy);
    float4 f4xs = (f4u - DEPTH_C.x) * f4zs / DEPTH_F.x;
    float4 f4ys = (f4v - DEPTH_C.y) * f4zs / DEPTH_F.y;
    float3 f3v0 = float3(f4xs.y - f4xs.w, f4ys.y - f4ys.w, f4zs.y - f4zs.w);
    float3 f3v1 = float3(f4xs.z - f4xs.x, f4ys.z - f4ys.x, f4zs.z - f4zs.x);
    float3 f3Norm = normalize(cross(f3v0, f3v1));
#if CONFIDENCE_OUT
    float3 f3View = normalize(float3(f4xs.x, f4ys.x, f4zs.x));
    float fAngleConfidence = dot(-f3View, f3Norm);
    if (fAngleConfidence <= fAngleThreshold) {
        tex_uavConfidence[u3DTid.xy] = 0.f;
        tex_uavNormal[u3DTid.xy] = 0.f;
        return;
    }
    fAngleConfidence =
        (fAngleConfidence - fAngleThreshold) / (1.f - fAngleThreshold);
    float2 f2UV = u3DTid.xy - DEPTH_C;
    float fImgRadiusConfidence =
        saturate((float)(DEPTH_C.y * DEPTH_C.y) / (dot(f2UV, f2UV) + 1.f));
    fImgRadiusConfidence = fImgRadiusConfidence * fImgRadiusConfidence;
    float fDepthConfidence = saturate(1.5f / (fMax * 10.f));
    tex_uavConfidence[u3DTid.xy] =
        float4(fAngleConfidence, fImgRadiusConfidence, fDepthConfidence,
            min(fAngleConfidence, min(fImgRadiusConfidence, fDepthConfidence)));
#endif
    tex_uavNormal[u3DTid.xy] = float4(f3Norm * .5f + .5f, 1.f);
}

//
//groupshared uint uDepth[9][9];
//float3 GetPos(uint2 u2uv, uint2 u2)
//{
//    float3 f3Pos;
//    f3Pos.z = uDepth[u2.x][u2.y] * -0.001f;
//    f3Pos.xy = float2(u2uv - DEPTH_C) * f3Pos.z / DEPTH_F;
//    return f3Pos;
//}
//
//[numthreads(8, 8, 1)]
//void main2(uint3 u3DTid : SV_DispatchThreadID, uint3 u3GTid : SV_GroupThreadID)
//{
//    if (all(u3DTid.xy < u2Reso)) {
//        uDepth[u3GTid.x][u3GTid.y] = tex_srvDepth.Load(int3(u3DTid.xy, 0));
//    }
//    if (all(u3DTid.xy < u2Reso - 1)) {
//        if (u3GTid.y == 7) {
//            uDepth[u3GTid.x][8] =
//                tex_srvDepth.Load(int3(u3DTid.x, u3DTid.y + 1, 0));
//        }
//        if (u3GTid.x == 7) {
//            uDepth[8][u3GTid.y] =
//                tex_srvDepth.Load(int3(u3DTid.x + 1, u3DTid.y, 0));
//        }
//    }
//    GroupMemoryBarrierWithGroupSync();
//    float4 f4Output = float4(-1.f, -1.f, -1.f, -1.f);
//    uint2 u2uv = u3DTid.xy;
//    uint2 u2 = u3GTid.xy;
//    if (all(u3DTid.xy < (u2Reso - 1)))
//    {
//        float3 f3P0 = GetPos(u2uv, u2);
//        float3 f3P1 = GetPos(u2uv + uint2(1, 0), u2 + uint2(1, 0)) - f3P0;
//        f3P0 = GetPos(u2uv + uint2(0, 1), u2 + uint2(0, 1)) - f3P0;
//        f3P0 = normalize(cross(f3P0, f3P1));
//        f4Output = float4(f3P0 * 0.5f + 0.5f, 1.f);
//    }
//    tex_uavNormal[u2uv] = f4Output;
//}
//
//groupshared half3 f3Pos[9][9];
//[numthreads(8, 8, 1)]
//void main1(uint3 u3DTid : SV_DispatchThreadID, uint3 u3GTid : SV_GroupThreadID)
//{
//    if (all(u3DTid.xy < u2Reso)) {
//        f3Pos[u3GTid.x][u3GTid.y] = GetValidPos(u3DTid.xy);
//    }
//    if (all(u3DTid.xy < u2Reso - 1)) {
//        if (u3GTid.y == 7) {
//            f3Pos[u3GTid.x][8] = GetValidPos(uint2(u3DTid.x, u3DTid.y + 1));
//        }
//        if (u3GTid.x == 7) {
//            f3Pos[8][u3GTid.y] = GetValidPos(uint2(u3DTid.x + 1, u3DTid.y));
//        }
//    }
//    GroupMemoryBarrierWithGroupSync();
//    half4 f4Output = half4(-1.f, -1.f, -1.f, -1.f);
//    if (all(u3DTid.xy < (u2Reso - 1)))
//    {
//        half3 f3P0 = f3Pos[u3GTid.x][u3GTid.y];
//        half3 f3P1 = f3Pos[u3GTid.x + 1][u3GTid.y] - f3P0;
//        f3P0 = f3Pos[u3GTid.x][u3GTid.y + 1] - f3P0;
//        f3P0 = normalize(cross(f3P0, f3P1));
//        f4Output = half4(f3P0 * 0.5f + 0.5f, 1.f);
//    }
//    tex_uavNormal[u3DTid.xy] = f4Output;
//}