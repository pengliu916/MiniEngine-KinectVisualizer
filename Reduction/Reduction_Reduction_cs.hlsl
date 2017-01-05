StructuredBuffer<TYPE> buf_srvInput : register(t0);
RWStructuredBuffer<TYPE> buf_uavOutput : register(u0);

//==============================================================================
// Note: in my case there are only 2 case for reduction: Depth which is 512x424,
// and Color which is 960x512(downsampled), since in both case I can't get final
// result in 1 pass, so I have to do 2 pass, and to avoid too much padding, I
// need to figured out some number: thread_per_group, fetch_per_group. Thus I
// got
//    thread_per_group * fetch_per_group * group_count >= pixel_count
//    thread_per_group * fetch_per_group * 1 >= group_count
// ==> thread_per_group * fetch_per_group ~ sqrt(pixel_count) which is 702
// ==> thread_per_group set to 128 fetch_per_group set to 8
//==============================================================================
#define THREAD 128
#define FETCH_COUNT 8
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
    sharedMem[GI] = buf_srvInput[uIdx]
        + buf_srvInput[uIdx + 1]
        + buf_srvInput[uIdx + 2]
        + buf_srvInput[uIdx + 3]
        + buf_srvInput[uIdx + 4]
        + buf_srvInput[uIdx + 5]
        + buf_srvInput[uIdx + 6]
        + buf_srvInput[uIdx + 7];
    GroupMemoryBarrierWithGroupSync();

    if (GI < 64) sharedMem[GI] += sharedMem[GI + 64];
    GroupMemoryBarrierWithGroupSync();

    if (GI < 32) sharedMem[GI] += sharedMem[GI + 32];
    if (GI < 16) sharedMem[GI] += sharedMem[GI + 16];
    if (GI < 8) sharedMem[GI] += sharedMem[GI + 8];
    if (GI < 4) sharedMem[GI] += sharedMem[GI + 4];
    if (GI < 2) sharedMem[GI] += sharedMem[GI + 2];
    if (GI < 1) sharedMem[GI] += sharedMem[GI + 1];
    
    if (GI == 0) {
        buf_uavOutput[uFinalResult ? uResultOffset : Gid.x] = sharedMem[0];
    }
}