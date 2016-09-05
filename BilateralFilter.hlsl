#include "SeparableFilter_SharedHeader.inl"
#define PI ( 3.1415927f )
// Cutoff at 2*deviation
#define GAUSSIAN_VARIANCE ((KERNEL_RADIUS+1.f)*(KERNEL_RADIUS+1.f)*0.25f)

Texture2D<uint>    inputTex  : register(t0);

#if QuadVS
void vs_quad_main(in uint VertID : SV_VertexID, 
    out float2 Tex : TEXCOORD0, out float4 Pos : SV_Position)
{
    // Texture coordinates range [0, 2], but only [0, 1] appears on screen.
    Tex = uint2(uint2(VertID, VertID << 1) & 2);
    Pos = float4(lerp(float2(-1, 1), float2(1, -1), Tex), 0, 1);
    Tex *= Reso;
}
#endif // QuadVS

#if FilterPS
float gaussian_weight(float offset)
{
    // Since we will divide by weight sum, the following line is unnecessary
    //float weight = 1.0f / sqrt( 2.0f * PI * GAUSSIAN_VARIANCE );
    return /*weight**/exp(-(offset * offset) / (2.f * GAUSSIAN_VARIANCE));
}

uint ps_bilateralfilter_main(float2 Tex : TEXCOORD0) : SV_Target
{
    int3 currentLocation = int3(Tex.xy,0);
    uint centerDepth = inputTex.Load(currentLocation);

    float Depth = 0;
    float Weight = 0.0f;

    [unroll]
    for (int i = -KERNEL_RADIUS; i <= KERNEL_RADIUS; ++i) {
#if HorizontalPass
        uint sampleDepth = inputTex.Load(currentLocation, int2(0, i));
#else
        uint sampleDepth = inputTex.Load(currentLocation, int2(i, 0));
#endif // HorizontalPass
        float weight = gaussian_weight(i);
        float delta = centerDepth - sampleDepth;
        // Multiply by range weight
        weight *= exp((-1.0f * delta * delta) / (2.0f * DiffrenceVariance));
        Depth += sampleDepth*weight;
        Weight += weight;
    }
    return (uint)(Depth / Weight + 0.5f);
}
#endif // FilterPS