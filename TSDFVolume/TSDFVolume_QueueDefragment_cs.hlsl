#include "TSDFVolume.inl"

RWStructuredBuffer<uint> buf_uavOccupiedBlocksBuf : register(u0);
RWByteAddressBuffer buf_uavFreedOccupiedBlocksBufCtr : register(u1);
StructuredBuffer<uint> buf_srvFreedOccupiedBlocksBuf : register(t0);
ByteAddressBuffer buf_srvIndirectJobParams : register(t1);
//------------------------------------------------------------------------------
// Compute Shader
//------------------------------------------------------------------------------
[numthreads(WARP_SIZE, 1, 1)]
void main(uint3 u3DTid : SV_DispatchThreadID)
{
    if (u3DTid.x >= buf_srvIndirectJobParams.Load(FREEQUEUE_STARTOFFSET)) {
        return;
    }
    // reset the freedQueue
    if (u3DTid.x == 0) {
        buf_uavFreedOccupiedBlocksBufCtr.Store(0, 0);
    }
    uint uOccupiedQEndIdx = buf_srvIndirectJobParams.Load(OCCUPIEDQUEUE_SIZE);
    uint uFreeIdx = buf_srvFreedOccupiedBlocksBuf[u3DTid.x];
    // Free slot idx is beyond new size, do nothing
    if (uFreeIdx > uOccupiedQEndIdx) {
        return;
    }
    uint jobIdx;
    uint jobValue;
    do {
        jobIdx = buf_uavOccupiedBlocksBuf.DecrementCounter();
        jobValue = buf_uavOccupiedBlocksBuf[jobIdx];
    } while (jobValue == BLOCKFREEDMASK);
    buf_uavOccupiedBlocksBuf[uFreeIdx] = jobValue;
}