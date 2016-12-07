// PS for rendering the stepinfo Texture for raymarching
// [Warning] keep this PS short since during alpha blending, each pixel will be
// shaded multiple times
float2 main(float4 f4ProjPos : SV_POSITION,
    float2 f2Depth: NORMAL0) : SV_Target
{
    // Alpha blend will store the smallest RG channel, so to keep track of both
    // min/max length, G channel will need to be multiplied by -1
#if DEBUG_VIEW
    return float2(1.f, 0.f);
#else
    return f2Depth;
#endif // !DEBUG_VIEW
}