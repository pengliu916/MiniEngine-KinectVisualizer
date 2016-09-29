#include "SeparableFilter.inl"

void main(in uint VertID : SV_VertexID, 
    out float2 Tex : TEXCOORD0, out float4 Pos : SV_Position)
{
    // Texture coordinates range [0, 2], but only [0, 1] appears on screen.
    Tex = uint2(uint2(VertID, VertID << 1) & 2);
    Pos = float4(lerp(float2(-1, 1), float2(1, -1), Tex), 0, 1);
    Tex *= u2Reso;
}