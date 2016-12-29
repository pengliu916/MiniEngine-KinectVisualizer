#include "SeparableFilter.inl"

#pragma warning(push)
// Compiler falsely emit x4000 if I do early out return
#pragma warning(disable: 4000)

Texture2D<uint> tex_srvInput : register(t0);
StructuredBuffer<float> buf_srvGaussianWeight : register(t1);
RWTexture2D<float> tex_uavWeight : register(u0);

uint main(float2 Tex : TEXCOORD0) : SV_Target
{
    int3 i3CurrentUVIdx = int3(Tex.xy,0);
    uint uCenterDepth = tex_srvInput.Load(i3CurrentUVIdx);
    if (uCenterDepth < 200 || uCenterDepth > 10000) {
        tex_uavWeight[Tex.xy] = 0;
        return 0;
    }

    float fDepth = 0;
    float fWeight = 0.0f;

    for (int i = -iKernelRadius; i <= iKernelRadius; ++i) {
#if HorizontalPass
        uint uSampleDepth = tex_srvInput.Load(i3CurrentUVIdx + int3(i, 0, 0));
#else
        uint uSampleDepth = tex_srvInput.Load(i3CurrentUVIdx + int3(0, i, 0));
#endif // HorizontalPass
        if (abs(i) <= 2 && abs((int)uSampleDepth - (int)uCenterDepth) > 100) {
            tex_uavWeight[Tex.xy] = 0.f;
            return 0;
        }
        float weight = buf_srvGaussianWeight.Load(abs(i));
        float delta = uCenterDepth - uSampleDepth;
        // Multiply by range weight
        weight *= exp((-1.0f * delta * delta) / (2.0f * fRangeVar));
        fDepth += uSampleDepth * weight;
        fWeight += weight;
    }
    return (uint)(fDepth / fWeight + 0.5f);
}
#pragma  warning(pop)