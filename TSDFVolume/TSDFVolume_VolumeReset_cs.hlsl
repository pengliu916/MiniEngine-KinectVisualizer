#include "TSDFVolume.inl"
#include "TSDFVolume.hlsli"

#if TSDFVOL_RESET
#if TYPED_UAV
RWBuffer<float> tex_uavTSDFVol : register(u0);
RWBuffer<float> tex_uavWeightVol : register(u1);
#endif // TYPED_UAV
#if TEX3D_UAV
RWTexture3D<float> tex_uavTSDFVol : register(u0);
RWTexture3D<uint> tex_uavWeightVol : register(u1);
#endif // TEX3D_UAV

[numthreads(THREAD_X, THREAD_Y, THREAD_Z)]
void main(uint3 u3DTid : SV_DispatchThreadID)
{
    BufIdx bufIdx = BUFFER_INDEX(u3DTid);
    tex_uavTSDFVol[bufIdx] = 1.f;
    tex_uavWeightVol[bufIdx] = 0;
}
#endif // TSDFVOL_RESET

#if FUSEBLOCKVOL_RESET
RWTexture3D<int> tex_uavFuseBlockVol : register(u1);

[numthreads(THREAD_X, THREAD_Y, THREAD_Z)]
void main(uint3 u3DTid : SV_DispatchThreadID)
{
    tex_uavFuseBlockVol[u3DTid] = 0;
}
#endif // FUSEBLOCKVOL_RESET

#if RENDERBLOCKVOL_RESET
RWTexture3D<uint> tex_uavRenderBlockVol : register(u0);

[numthreads(THREAD_X, THREAD_Y, THREAD_Z)]
void main(uint3 u3DTid : SV_DispatchThreadID)
{
    tex_uavRenderBlockVol[u3DTid] = 0;
}
#endif // RENDERBLOCKVOL_RESET