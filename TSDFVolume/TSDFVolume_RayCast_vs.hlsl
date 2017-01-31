#include "TSDFVolume.inl"
#include "TSDFVolume.hlsli"

// Index 0: viewMatrixInv, Index 1: viewMatrix
StructuredBuffer<matrix> buf_srvSensorMatrices : register(t0);
#if FOR_SENSOR
#include "CalibData.inl"

void main(uint uVertexID : SV_VertexID, out float3 f3Pos : POSITION1,
    out float4 f4ProjPos : SV_Position)
{
    float2 f2Tex = float2(uint2(uVertexID, uVertexID << 1) & 2);
    f4ProjPos =
        float4(lerp(float2(-1.f, 1.f), float2(1.f, -1.f), f2Tex), 0.f, 1.f);
    float2 f2TLCorner = float2(0.f, 0.f);
    float2 f2BRCorner = float2(i2DepthReso);

    f2TLCorner = (f2TLCorner - DEPTH_C) * f2DepthRange.x / DEPTH_F;
    f2BRCorner = (f2BRCorner - DEPTH_C) * f2DepthRange.x / DEPTH_F;

    float4 f4Pos =
        float4(lerp(f2TLCorner, f2BRCorner, f2Tex), f2DepthRange.x, 1.f);

    f3Pos = mul(buf_srvSensorMatrices[0], f4Pos).xyz;
    //f3Pos = mul(mDepthViewInv, f4Pos).xyz;
}
#endif // FOR_SENSOR

#if FOR_VCAMERA
void main(uint uVertexID : SV_VertexID, out float3 f3Pos : POSITION1,
    out float4 f4ProjPos : SV_Position)
{
    float2 f2Tex = float2(uint2(uVertexID, uVertexID << 1) & 2);
    f4ProjPos =
        float4(lerp(float2(-1.f, 1.f), float2(1.f, -1.f), f2Tex), 0, 1);
    f3Pos = float3(f4ProjPos.xy *
        float2(fWideHeightRatio * fTanHFov * fClipDist,
            fTanHFov * fClipDist), -fClipDist);
    f3Pos = mul(mViewInv, float4(f3Pos, 1.f)).xyz;
}
#endif // FOR_VCAMERA
