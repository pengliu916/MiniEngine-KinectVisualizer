#include "PointCloudRenderer.inl"

Texture2D<uint> DepthMap : register(t0);

static const uint2 offset[4] = {
    uint2(0,0),
    uint2(1,0),
    uint2(0,1),
    uint2(1,1)
};

struct TexColPos {
    float2 Tex : TEXCOORD0;
#if SHADED
    float3 Col : COLOR0;
#endif // SHADED
    float4 Pos : SV_Position;
};

#if SHADED
float3 Shade(float4 Pos, float3 Nor)
{
    // shading part
    float4 light_dir = f4LightPos - Pos;
    float light_dist = length(light_dir);
    light_dir /= light_dist;
    light_dir.w = clamp(1.0f / (f4LightAttn.x + f4LightAttn.y * light_dist +
        f4LightAttn.z * light_dist * light_dist), 0, 1);
    //float angleAttn = clamp( dot( Nor, light_dir.xyz ), 0, 1 );
    float angleAttn = abs(dot(Nor, light_dir.xyz));
    return  light_dir.w * angleAttn;// +f4AmbientCol.xyz;
}
#endif // SHADED

// Use GS for this is not optimal. Best solution should be using indexed 
// heighmap mesh in VS for vertex reuse
[maxvertexcount(4)]
void main(point uint VertID[1] : POSITION0,
    inout TriangleStream<TexColPos> TriStream)
{
    TexColPos output;
    uint2 xy = uint2(VertID[0] % f2DepthInfraredReso.x,
        VertID[0] / f2DepthInfraredReso.x);
    uint z[4] = {DepthMap[xy + offset[0]], DepthMap[xy + offset[1]],
        DepthMap[xy + offset[2]], DepthMap[xy + offset[3]]};
    uint2 minmax = z[0] > z[1] ? uint2(z[1], z[0]) : uint2(z[0], z[1]);
    minmax.x = min(min(minmax.x, z[2]), z[3]);
    minmax.y = max(max(minmax.y, z[2]), z[3]);

    if ((minmax.y - minmax.x)*0.001f > fQuadZSpanThreshold) {
        return;
    }

    float4 Pos[4];
    float4 projectedPos[4];
    float2 Tex[4];
    [unroll] for (int i = 0; i < 4; i++) {
        uint2 _xy = xy + offset[i];
        float depth = z[i] * 0.001f;
        Pos[i] = float4((_xy - f4DepthCxyFxy.xy) /
            f4DepthCxyFxy.zw * depth, depth, 1.f);
#if SHADED
        float4 pos_col = mul(mDepth2Color, Pos[i]);
        Tex[i] = pos_col.xy / pos_col.z * f4ColorCxyFxy.zw + f4ColorCxyFxy.xy;
#endif // SHADED
        Pos[i] = Pos[i] * float4(1.f, -1.f, 1.f, 1.f) + f4Offset;
        projectedPos[i] = mul(mViewProj, Pos[i]);
    }
#if SHADED
    float3 nor[4] = {Pos[1].xyz - Pos[0].xyz, Pos[2].xyz - Pos[0].xyz,
        Pos[1].xyz - Pos[3].xyz, Pos[2].xyz - Pos[3].xyz};
    nor[0] = normalize(cross(nor[0], nor[1]));
    nor[3] = normalize(cross(nor[2], nor[3]));
    nor[1] = nor[2] = normalize(nor[0] + nor[3]);
#endif // SHADED
    [unroll] for (int j = 0; j < 4; j++) {
        output.Pos = projectedPos[j];
        output.Tex = Tex[j];
#if SHADED
        output.Col = Shade(Pos[j], nor[j]);
#endif // SHADED
        TriStream.Append(output);
    }
    TriStream.RestartStrip();
}