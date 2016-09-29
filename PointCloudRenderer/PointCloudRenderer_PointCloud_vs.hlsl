#include "PointCloudRenderer.inl"

Texture2D<uint> DepthMap : register(t0);

// [NOTE] when use 'out' keyword: the order of next stage input should match 
// previous stage out order
void main(in uint VertID : SV_VertexID,
    out float2 Tex : TEXCOORD0, out float4 Pos : SV_Position)
{
    uint2 xy =
        uint2(VertID % f2DepthInfraredReso.x, VertID / f2DepthInfraredReso.x);
    float z = DepthMap[xy] * 0.001f;
    float4 pos = float4((xy - f4DepthCxyFxy.xy) / f4DepthCxyFxy.zw * z, z, 1.f);
    float4 pos_col = mul(mDepth2Color, pos);
    Tex = pos_col.xy / pos_col.z * f4ColorCxyFxy.zw + f4ColorCxyFxy.xy;
    pos = pos * float4(1.f, -1.f, 1.f, 1.f) + f4Offset;
    Pos = mul(mViewProj, pos);
}