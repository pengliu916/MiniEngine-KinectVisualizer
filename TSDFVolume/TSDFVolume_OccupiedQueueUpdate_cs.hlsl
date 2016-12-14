
#include "TSDFVolume.inl"

#if PREPARE // Split new/free queue into 2 part
RWByteAddressBuffer buf_uavNewOccupiedBlockBufCtr : register(u0);
RWByteAddressBuffer buf_uavFreedOccupiedBlocksBufCtr : register(u1);
RWByteAddressBuffer buf_uavIndirectParam : register(u2);
RWByteAddressBuffer buf_uavIndirectJobParams : register(u3);
ByteAddressBuffer buf_srvOccupiedBlockBufCtr : register(t0);
//------------------------------------------------------------------------------
// Compute Shader
//------------------------------------------------------------------------------
[numthreads(1, 1, 1)]
void main(uint uGIdx : SV_GroupIndex)
{
    // If Freed slot ctr is larger than new blocks ctr
    uint uNewBlockCtr = buf_uavNewOccupiedBlockBufCtr.Load(0);
    uint uFreedBlockCtr = buf_uavFreedOccupiedBlocksBufCtr.Load(0);
    if (uNewBlockCtr <= uFreedBlockCtr) {
        uint uLeftOver = uFreedBlockCtr - uNewBlockCtr;
        buf_uavIndirectJobParams.Store(FREEQUEUE_STARTOFFSET, uLeftOver);
        buf_uavIndirectJobParams.Store(FREEQUEUE_JOBCOUNT, uNewBlockCtr);
        buf_uavIndirectJobParams.Store(ADDQUEUE_STARTOFFSET, 0);
        buf_uavIndirectJobParams.Store(ADDQUEUE_JOBCOUNT, 0);
        // Prepare information for defragmentation
        if (uLeftOver > (uint)iDefragmentThreshold) {
            uint uNewSize = buf_srvOccupiedBlockBufCtr.Load(0) - uLeftOver;
            buf_uavIndirectJobParams.Store(OCCUPIEDQUEUE_SIZE, uNewSize);
            uint uPaddedCtr = (uLeftOver + WARP_SIZE - 1) & ~(WARP_SIZE - 1);
            buf_uavIndirectParam.Store(48, uPaddedCtr >> 5);
        } else {
            buf_uavIndirectParam.Store(48, 0);
        }
        uint uPaddedCtr = (uNewBlockCtr + WARP_SIZE - 1) & ~(WARP_SIZE - 1);
        buf_uavIndirectParam.Store(24, uPaddedCtr >> 5);// uPaddedCtr/WARP_SIZE
        buf_uavIndirectParam.Store(36, 0);
        buf_uavFreedOccupiedBlocksBufCtr.Store(0, uLeftOver);
        buf_uavNewOccupiedBlockBufCtr.Store(0, 0);
    } else {
        uint uLeftOver = uNewBlockCtr - uFreedBlockCtr;
        buf_uavIndirectJobParams.Store(FREEQUEUE_STARTOFFSET, 0);
        buf_uavIndirectJobParams.Store(FREEQUEUE_JOBCOUNT, uFreedBlockCtr);
        buf_uavIndirectJobParams.Store(ADDQUEUE_STARTOFFSET, uFreedBlockCtr);
        buf_uavIndirectJobParams.Store(ADDQUEUE_JOBCOUNT, uLeftOver);
        uint uPaddedCtr = (uFreedBlockCtr + WARP_SIZE - 1) & ~(WARP_SIZE - 1);
        buf_uavIndirectParam.Store(24, uPaddedCtr >> 5);// uPaddedCtr/WARP_SIZE
        uPaddedCtr = (uLeftOver + WARP_SIZE - 1) & ~(WARP_SIZE - 1);
        buf_uavIndirectParam.Store(36, uPaddedCtr >> 5);
        buf_uavFreedOccupiedBlocksBufCtr.Store(0, 0);
        buf_uavNewOccupiedBlockBufCtr.Store(0, 0);
        buf_uavIndirectParam.Store(48, 0);
    }
}
#endif

#if PASS_1 // Fill in Free queue
RWStructuredBuffer<uint> buf_uavOccupiedBlocksBuf : register(u0);
StructuredBuffer<uint> buf_srvNewOccupiedBlockBuf : register(t0);
StructuredBuffer<uint> buf_srvFreedOccupiedBlocksBuf : register(t1);
ByteAddressBuffer buf_srvIndirectJobParams : register(t2);
//------------------------------------------------------------------------------
// Compute Shader
//------------------------------------------------------------------------------
[numthreads(WARP_SIZE, 1, 1)]
void main(uint3 u3DTid : SV_DispatchThreadID)
{
    if (u3DTid.x >= buf_srvIndirectJobParams.Load(FREEQUEUE_JOBCOUNT)) {
        return;
    }
    uint uOffset = buf_srvIndirectJobParams.Load(FREEQUEUE_STARTOFFSET);
    uint uIdx = buf_srvFreedOccupiedBlocksBuf[u3DTid.x + uOffset];
    buf_uavOccupiedBlocksBuf[uIdx] = buf_srvNewOccupiedBlockBuf[u3DTid.x];
}
#endif

#if PASS_2 // Append to Occupied queue
RWStructuredBuffer<uint> buf_uavOccupiedBlocksBuf : register(u0);
StructuredBuffer<uint> buf_srvNewOccupiedBlockBuf : register(t0);
ByteAddressBuffer buf_srvIndirectJobParams : register(t1);
//------------------------------------------------------------------------------
// Compute Shader
//------------------------------------------------------------------------------
[numthreads(WARP_SIZE, 1, 1)]
void main(uint3 u3DTid : SV_DispatchThreadID)
{
    if (u3DTid.x >= buf_srvIndirectJobParams.Load(ADDQUEUE_JOBCOUNT)) {
        return;
    }
    uint uOffset = buf_srvIndirectJobParams.Load(ADDQUEUE_STARTOFFSET);
    uint uPackedBlockIdx =
        buf_srvNewOccupiedBlockBuf[uOffset + u3DTid.x];
    uint uIdx = buf_uavOccupiedBlocksBuf.IncrementCounter();
    buf_uavOccupiedBlocksBuf[uIdx] = uPackedBlockIdx;
}
#endif

#if RESOLVE // Based on OccupiedBlock ctr, update indirect param for dispatch
RWByteAddressBuffer buf_uavIndirectParam : register(u0);
ByteAddressBuffer buf_srvOccupiedBlocksBufCtr : register(t0);
//------------------------------------------------------------------------------
// Compute Shader
//------------------------------------------------------------------------------
[numthreads(1, 1, 1)]
void main(uint uGIdx : SV_GroupIndex)
{
    if (uGIdx != 0) {
        return;
    }
    uint uPaddedCtr = (buf_srvOccupiedBlocksBufCtr.Load(0) + WARP_SIZE - 1)
        & ~(WARP_SIZE - 1);
    uint uNumThreadGroupX = uPaddedCtr >> 5;
    buf_uavIndirectParam.Store(0, uNumThreadGroupX);
}
#endif