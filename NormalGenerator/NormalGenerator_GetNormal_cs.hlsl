#include "NormalGenerator.inl"
#include "CalibData.inl"

Texture2D<uint> tex_srvDepth : register(t0);
RWTexture2D<float4> tex_uavNormal : register(u0);

float3 GetValidPos(uint2 u2uv)
{
    float3 f3Pos;
    f3Pos.z = tex_srvDepth.Load(int3(u2uv, 0)) * -0.001f;
    f3Pos.xy = float2(u2uv - DEPTH_C) * f3Pos.z / DEPTH_F;
    return f3Pos;
}

[numthreads(8, 8, 1)]
void main(uint3 u3DTid : SV_DispatchThreadID)
{
    float4 f4Output = float4(-1.f, -1.f, -1.f, -1.f);
    uint2 u2uv = uint2(u3DTid.xy);
    if (all(u3DTid.xy < (u2Reso - 1)))
    {
        float3 f3temp = GetValidPos(u2uv);
        float3 f3v0 = GetValidPos(u2uv + uint2(1, 0)) - f3temp;
        float3 f3v1 = GetValidPos(u2uv + uint2(0, 1)) - f3temp;
        f3temp = normalize(cross(f3v1, f3v0));
        f4Output = float4(f3temp * 0.5f + 0.5f, 1.f);
    }
    tex_uavNormal[u2uv] = f4Output;
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