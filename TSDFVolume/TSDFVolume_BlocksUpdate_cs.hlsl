#include "TSDFVolume_VoxelUpdate.hlsli"

RWTexture3D<uint> tex_uavFuseBlockVol : register(u2);

RWStructuredBuffer<uint> buf_uavNewOccupiedBlocksBuf : register(u3);
RWStructuredBuffer<uint> buf_uavFreedOccupiedBlocksBuf : register(u4);
RWStructuredBuffer<uint> buf_uavOccupiedBlocksBuf : register(u5);
RWTexture3D<uint> tex_uavRenderBlockVol : register(u6);
StructuredBuffer<uint> buf_srvUpdateBlockQueue : register(t3);

//------------------------------------------------------------------------------
// Compute Shader
//------------------------------------------------------------------------------
// Each thread is updating one voxel, one block contains uTGPerFuseBlock of
// threadgroup. One thread of each threadgroup will do an atomic update to
// BlockVolume[block].occupied (to reduce contention)
groupshared uint uOccupiedTG = 0;
groupshared uint uEmptyThread = 0;
[numthreads(THREAD_DIM, THREAD_DIM, THREAD_DIM)]
void main(uint3 u3GID : SV_GroupID, uint3 u3GTID : SV_GroupThreadID,
    uint uGIdx : SV_GroupIndex)
{
    uint uWorkQueueIdx = u3GID.x / uTGPerFuseBlock;
    uint uThreadGroupIdxInBlock = u3GID.x % uTGPerFuseBlock;
    uint3 u3ThreadGroupIdxInBlock =
        MakeU3Idx(uThreadGroupIdxInBlock, uTGFuseBlockRatio);
    uint3 u3BlockIdx = UnpackedToUint3(buf_srvUpdateBlockQueue[uWorkQueueIdx]);
    uint3 u3VolumeIdx = u3BlockIdx * vParam.uVoxelFuseBlockRatio +
        u3ThreadGroupIdxInBlock * THREAD_DIM + u3GTID;

    bool bEmpty;
    // uNumemptyThreadGroup values for blocks in update queue are guaranteed to
    // be either 0 (full occupied) or uTGPerFuseBlock (empty) right before
    // this pass
    uint uNumEmptyTG =
        tex_uavFuseBlockVol[u3BlockIdx] & BLOCKSTATEMASK_OCCUPIED;
    if (UpdateVoxel(u3VolumeIdx, bEmpty)) {
        if (bEmpty) {
            InterlockedAdd(uEmptyThread, 1);
        } else {
            uOccupiedTG = 1;
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // Need only one thread in threadgroup to do the following, so early out
    if (uGIdx != 0) {
        return;
    }

    if (uNumEmptyTG == 0
        && uEmptyThread == THREAD_DIM * THREAD_DIM * THREAD_DIM) {
        // Occupied Block is freed, add block to free block queue
        uint uOrig = 0;
        InterlockedAdd(tex_uavFuseBlockVol[u3BlockIdx], 1, uOrig);
        if ((uOrig & BLOCKSTATEMASK_OCCUPIED) == uTGPerFuseBlock - 1) {
            uint uFreeQueueIdx =
                buf_uavFreedOccupiedBlocksBuf.IncrementCounter();
            uint uOccupiedQIdx = uOrig >> BLOCKSTATEMASK_IDXOFFSET;
            buf_uavFreedOccupiedBlocksBuf[uFreeQueueIdx] = uOccupiedQIdx;
            buf_uavOccupiedBlocksBuf[uOccupiedQIdx] = BLOCKFREEDMASK;
            // Update render block state
            InterlockedAdd(tex_uavRenderBlockVol[
                u3VolumeIdx / vParam.uVoxelRenderBlockRatio], -1);
        }
    } else if (uNumEmptyTG == uTGPerFuseBlock && uOccupiedTG) {
        // Empty Block found surface, add block to new occupied block queue
        uint uOrig = 1000;
        InterlockedAdd(tex_uavFuseBlockVol[u3BlockIdx], -1, uOrig);
        if ((uOrig & BLOCKSTATEMASK_OCCUPIED) == uTGPerFuseBlock) {
            uint uNewQueueIdx = buf_uavNewOccupiedBlocksBuf.IncrementCounter();
            buf_uavNewOccupiedBlocksBuf[uNewQueueIdx] =
                PackedToUint(u3BlockIdx);
            // Update render block state
            InterlockedAdd(tex_uavRenderBlockVol[
                u3VolumeIdx / vParam.uVoxelRenderBlockRatio], 1);
        }
    }
    // Reset block's update status for next update iteration
    // Moved to a separated cs to avoid 'weird bug' and atomic ops 
    //if (uThreadGroupIdxInBlock == 0 && uGIdx == 0) {
    //    InterlockedAnd(tex_uavDebugFuseBlockVol[u3BlockIdx], ~1);
    //    InterlockedAnd(
    //        tex_uavFuseBlockVol[u3BlockIdx], ~BLOCKSTATEMASK_UPDATE);
    //}
}