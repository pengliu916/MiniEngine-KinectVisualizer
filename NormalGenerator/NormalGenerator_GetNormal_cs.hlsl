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

[numthreads(8 ,8, 1)]
void main(uint3 u3DTid : SV_DispatchThreadID)
{
    float4 f4Output = float4(-1.f, -1.f, -1.f, -1.f);
    uint2 u2uv = uint2(u3DTid.xy);
    if (all(u3DTid.xy < (u2Reso - 1)))
    {
        float3 f3Pos0 = GetValidPos(u2uv);
        float3 f3Pos1 = GetValidPos(u2uv + uint2(1, 0));
        float3 f3Pos2 = GetValidPos(u2uv + uint2(0, 1));
        float3 f3Nor = normalize(cross(f3Pos2 - f3Pos0, f3Pos1 - f3Pos0));
        f4Output = float4(f3Nor * 0.5f + 0.5f, 1.f);
    }
    tex_uavNormal[u2uv] = f4Output;
}