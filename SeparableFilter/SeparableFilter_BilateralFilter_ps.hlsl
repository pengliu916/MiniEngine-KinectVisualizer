#include "SeparableFilter.inl"

#pragma warning(push)
// Compiler falsely emit x4000 if I do early out return
#pragma warning(disable: 4000)

Texture2D<uint> inputTex : register(t0);
StructuredBuffer<float> bufGaussianWeight : register(t1);

uint main(float2 Tex : TEXCOORD0) : SV_Target
{
    int3 i3CurrentUVIdx = int3(Tex.xy,0);
    uint uCenterDepth = inputTex.Load(i3CurrentUVIdx);

    float fDepth = 0;
    float fWeight = 0.0f;

    for (int i = -iKernelRadius; i <= iKernelRadius; ++i) {
#if HorizontalPass
        uint uSampleDepth = inputTex.Load(i3CurrentUVIdx + int3(0, i, 0));
#else
        uint uSampleDepth = inputTex.Load(i3CurrentUVIdx + int3(i, 0, 0));
#endif // HorizontalPass
        float weight = bufGaussianWeight.Load(abs(i));
        float delta = uCenterDepth - uSampleDepth;
        // Multiply by range weight
        weight *= exp((-1.0f * delta * delta) / (2.0f * fRangeVar));
        fDepth += uSampleDepth * weight;
        fWeight += weight;
    }
    return (uint)(fDepth / fWeight + 0.5f);
}
#pragma  warning(pop)