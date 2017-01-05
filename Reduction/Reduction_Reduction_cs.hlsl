StructuredBuffer<TYPE> buf_srvInput : register(t0);
RWStructuredBuffer<TYPE> buf_uavOutput : register(u0);

cbuffer CBdata : register(b0)
{
    uint uResultOffset;
    uint uFinalResult;
}

groupshared TYPE sharedMem[THREAD];

[numthreads(THREAD, 1, 1)]
void main(uint3 Gid : SV_GroupID, uint3 DTid : SV_DispatchThreadID,
    uint GI : SV_GroupIndex)
{
    uint uIdx = DTid.x * FETCH_COUNT;
    sharedMem[GI] = buf_srvInput[uIdx];
    [unroll(FETCH_COUNT - 1)]
    for (int i = 1; i < FETCH_COUNT; ++i) {
        sharedMem[GI] += buf_srvInput[uIdx + i];
    }
    GroupMemoryBarrierWithGroupSync();
#if THREAD >=1024
    if (GI < 512) sharedMem[GI] += sharedMem[GI + 512];
    GroupMemoryBarrierWithGroupSync();
#endif
#if THREAD >= 512
    if (GI < 256) sharedMem[GI] += sharedMem[GI + 256];
    GroupMemoryBarrierWithGroupSync();
#endif
#if THREAD >= 256
    if (GI < 128) sharedMem[GI] += sharedMem[GI + 128];
    GroupMemoryBarrierWithGroupSync();
#endif
#if THREAD >= 128
    if (GI < 64) sharedMem[GI] += sharedMem[GI + 64];
    GroupMemoryBarrierWithGroupSync();
#endif
#if THREAD >= 64
    if (GI < 32) sharedMem[GI] += sharedMem[GI + 32];
#endif
#if THREAD >= 32
    if (GI < 16) sharedMem[GI] += sharedMem[GI + 16];
#endif
    if (GI < 8) sharedMem[GI] += sharedMem[GI + 8];
    if (GI < 4) sharedMem[GI] += sharedMem[GI + 4];
    if (GI < 2) sharedMem[GI] += sharedMem[GI + 2];
    if (GI < 1) sharedMem[GI] += sharedMem[GI + 1];
    
    if (GI == 0) {
        buf_uavOutput[uFinalResult ? uResultOffset : Gid.x] = sharedMem[0];
    }
}