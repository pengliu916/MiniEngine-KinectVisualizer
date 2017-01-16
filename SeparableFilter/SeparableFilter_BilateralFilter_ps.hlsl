#include "SeparableFilter.inl"

#pragma warning(push)
// Compiler falsely emit x4000 if I do early out return
#pragma warning(disable: 4000)

Texture2D<float> tex_srvNormInput : register(t0);
StructuredBuffer<float> buf_srvGaussianWeight : register(t1);
RWTexture2D<float> tex_uavWeight : register(u0);

float main(float2 Tex : TEXCOORD0) : SV_Target
{
    int3 i3CurrentUVIdx = int3(Tex.xy,0);
    float fCenterNormDepth = tex_srvNormInput.Load(i3CurrentUVIdx);
    if (fCenterNormDepth == 0.f) {
        tex_uavWeight[Tex.xy] = 0;
        return 0.f;
    }

    float fNormDepth = 0;
    float fWeight = 0.0f;

    for (int i = -iKernelRadius; i <= iKernelRadius; ++i) {
#if HorizontalPass
        float fSampleNormDepth =
            tex_srvNormInput.Load(i3CurrentUVIdx + int3(i, 0, 0));
#else
        float fSampleNormDepth =
            tex_srvNormInput.Load(i3CurrentUVIdx + int3(0, i, 0));
#endif // HorizontalPass
#if EdgeRemoval
        // absolute value is diff of norm depth so threshold need * 0.1f
        if (fSampleNormDepth == 0.f ||
            abs(fSampleNormDepth - fCenterNormDepth) > fEdgeThreshold * 0.1f) {
            tex_uavWeight[Tex.xy] = 0.f;
            return 0.f;
        }
#else
        if (fSampleNormDepth == 0.f) {
            tex_uavWeight[Tex.xy] = 0.f;
            return 0.f;
        }
#endif // EdgeRemoval
        float weight = buf_srvGaussianWeight.Load(abs(i));
        float delta = fCenterNormDepth - fSampleNormDepth;
        // Multiply by range weight
        // Since delta is normdepth, thus times 10.f
        weight *= exp((-100.0f * delta * delta) / (2.f * fRangeVar));
        fNormDepth += fSampleNormDepth * weight;
        fWeight += weight;
    }
    return fNormDepth / fWeight;
}
#pragma  warning(pop)