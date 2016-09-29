#include "SeparableFilter.inl"
#define PI ( 3.1415927f )
// Cutoff at 2*deviation
#define GAUSSIAN_VARIANCE ((KERNEL_RADIUS+1.f)*(KERNEL_RADIUS+1.f)*0.25f)

Texture2D<uint> inputTex : register(t0);

float gaussian_weight(float offset)
{
    // Since we will divide by weight sum, the following line is unnecessary
    //float weight = 1.0f / sqrt( 2.0f * PI * GAUSSIAN_VARIANCE );
    return /*weight**/exp(-(offset * offset) / (2.f * GAUSSIAN_VARIANCE));
}

uint main(float2 Tex : TEXCOORD0) : SV_Target
{
    int3 i3CurrentUVIdx = int3(Tex.xy,0);
    uint uCenterDepth = inputTex.Load(i3CurrentUVIdx);

    float fDepth = 0;
    float fWeight = 0.0f;

    [unroll]
    for (int i = -KERNEL_RADIUS; i <= KERNEL_RADIUS; ++i) {
#if HorizontalPass
        uint uSampleDepth = inputTex.Load(i3CurrentUVIdx, int2(0, i));
#else
        uint uSampleDepth = inputTex.Load(i3CurrentUVIdx, int2(i, 0));
#endif // HorizontalPass
        float weight = gaussian_weight(i);
        float delta = uCenterDepth - uSampleDepth;
        // Multiply by range weight
        weight *= exp((-1.0f * delta * delta) / (2.0f * fGaussianVar));
        fDepth += uSampleDepth*weight;
        fWeight += weight;
    }
    return (uint)(fDepth / fWeight + 0.5f);
}