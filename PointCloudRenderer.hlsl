#include "PointCloudRenderer_SharedHeader.inl"

Texture2D<uint> DepthMap : register(t0);
Texture2D<float4> ColorMap : register(t1);

static const uint2 offset[4] = {
    uint2(0,0),
    uint2(1,0),
    uint2(0,1),
    uint2(1,1)
};

#if HeightMapVS
// [NOTE] when use 'out' keyword: the order of next stage input should match 
// previous stage out order
void vs_heightmap_main(in uint VertID : SV_VertexID,
    out float2 Tex : TEXCOORD0, out float4 Pos : SV_Position)
{
    uint2 xy =
        uint2(VertID % DepthInfraredReso.x, VertID / DepthInfraredReso.x);
    float z = DepthMap[xy] * 0.001f;
    float4 pos = float4((xy - DepthCxyFxy.xy) / DepthCxyFxy.zw * z, z, 1.f);
    float4 pos_col = mul(Depth2Color, pos);
    Tex = pos_col.xy / pos_col.z * ColorCxyFxy.zw + ColorCxyFxy.xy;
    pos = pos * float4(1.f, -1.f, 1.f, 1.f) + Offset;
    Pos = mul(ViewProjMat, pos);
}
#endif // HeightMapVS

#if PassThroughVS
uint vs_passthrough_main(in uint VertID : SV_VertexID) : POSITION0
{
    return VertID;
}
#endif // PassThroughVS

#if NormalSurfaceGS
struct TexColPos {
    float2 Tex : TEXCOORD0;
    float3 Col : COLOR0;
    float4 Pos : SV_Position;
};

float3 Shade(float4 Pos, float3 Nor)
{
    // shading part
    float4 light_dir = LightPos - Pos;
    float light_dist = length(light_dir);
    light_dir /= light_dist;
    light_dir.w = clamp(1.0f / (LightAttn.x + LightAttn.y * light_dist +
        LightAttn.z * light_dist * light_dist), 0, 1);
    //float angleAttn = clamp( dot( Nor, light_dir.xyz ), 0, 1 );
    float angleAttn = abs(dot(Nor, light_dir.xyz));
    return  light_dir.w * angleAttn;// +AmbientCol.xyz;
}

// Use GS for this is not optimal. Best solution should be using indexed 
// heighmap mesh in VS for vertex reuse
[maxvertexcount(6)]
void gs_normalsurface_main(point uint VertID[1] : POSITION0,
    inout TriangleStream<TexColPos> TriStream)
{
    TexColPos output;
    uint2 xy = uint2(VertID[0] % DepthInfraredReso.x,
        VertID[0] / DepthInfraredReso.x);
    uint z[4] = {DepthMap[xy + offset[0]],
        DepthMap[xy + offset[1]],
        DepthMap[xy + offset[2]],
        DepthMap[xy + offset[3]]};
    uint2 minmax = z[0] > z[1] ? uint2(z[1], z[0]) : uint2(z[0], z[1]);
    minmax.x = min(min(minmax.x, z[2]), z[3]);
    minmax.y = max(max(minmax.y, z[2]), z[3]);

    if ((minmax.y - minmax.x)*0.001f > MaxQuadDepDiff) {
        return;
    }

    float4 Pos[4];
    float4 projectedPos[4];
    float2 Tex[4];
    [unroll] for (int i = 0; i < 4; i++) {
        uint2 _xy = xy + offset[i];
        float depth = z[i] * 0.001f;
        Pos[i] = float4((_xy - DepthCxyFxy.xy) /
            DepthCxyFxy.zw * depth, depth, 1.f);
        float4 pos_col = mul(Depth2Color, Pos[i]);
        Pos[i] = Pos[i] * float4(1.f, -1.f, 1.f, 1.f) + Offset;
        Tex[i] = pos_col.xy / pos_col.z * ColorCxyFxy.zw + ColorCxyFxy.xy;
        projectedPos[i] = mul(ViewProjMat, Pos[i]);
    }
    float3 e0 = Pos[1].xyz - Pos[0].xyz;
    float3 e1 = Pos[2].xyz - Pos[0].xyz;
    float3 nor = normalize(cross(e0, e1));
    [unroll] for (int j = 0; j < 3; j++) {
        output.Pos = projectedPos[j];
        output.Tex = Tex[j];
        output.Col = Shade(Pos[j], nor);
        TriStream.Append(output);
    }
    TriStream.RestartStrip();
    e0 = Pos[1].xyz - Pos[3].xyz;
    e1 = Pos[2].xyz - Pos[3].xyz;
    nor = normalize(cross(e1, e0));
    [unroll] for (int k = 1; k < 4; k++) {
        output.Pos = projectedPos[k];
        output.Tex = Tex[k];
        output.Col = Shade(Pos[k], nor);
        TriStream.Append(output);
    }
    TriStream.RestartStrip();
}
#endif // NormalSurfaceGS

#if SurfaceGS
struct TexPos {
    float2 Tex : TEXCOORD0;
    float4 Pos : SV_Position;
};

// Use gs for this is not optimal. Best solution should be using indexed 
// heighmap mesh in VS for vertex reuse
[maxvertexcount(4)]
void gs_surface_main(point uint VertID[1] : POSITION0, 
    inout TriangleStream<TexPos> TriStream)
{
    TexPos output;
    uint2 xy = uint2(VertID[0] % DepthInfraredReso.x,
        VertID[0] / DepthInfraredReso.x);
    uint z[4] = {DepthMap[xy + offset[0]],
        DepthMap[xy + offset[1]],
        DepthMap[xy + offset[2]],
        DepthMap[xy + offset[3]]};
    uint2 minmax = z[0] > z[1] ? uint2(z[1], z[0]) : uint2(z[0], z[1]);
    minmax.x = min(min(minmax.x, z[2]), z[3]);
    minmax.y = max(max(minmax.y, z[2]), z[3]);

    if ((minmax.y - minmax.x)*0.001f > MaxQuadDepDiff) {
        return;
    }

    [unroll] for (int i = 0; i < 4; i++) {
        uint2 _xy = xy + offset[i];
        float depth = z[i] * 0.001f;
        float4 pos = float4((_xy - DepthCxyFxy.xy) /
            DepthCxyFxy.zw * depth, depth, 1.f);
        float4 pos_col = mul(Depth2Color, pos);
        pos = pos * float4(1.f, -1.f, 1.f, 1.f) + Offset;
        output.Tex = pos_col.xy / pos_col.z * ColorCxyFxy.zw + ColorCxyFxy.xy;
        output.Pos = mul(ViewProjMat, pos);
        TriStream.Append(output);
    }
    TriStream.RestartStrip();
}
#endif // SurfaceGS

#if TexPS
float4 ps_tex_main(float2 Tex : TEXCOORD0) : SV_Target0
{
    return ColorMap[Tex];
}
#endif // TexPS

#if TexNormalPS
float4 ps_texnormal_main(float2 Tex : TEXCOORD0, float3 Col : COLOR0) 
    : SV_Target0
{
    return ColorMap[Tex] * float4(Col,1.f);
}
#endif // TexNormalPS