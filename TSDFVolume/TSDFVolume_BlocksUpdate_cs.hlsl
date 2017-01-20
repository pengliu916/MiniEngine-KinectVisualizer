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
// Each thread is updating one voxel, one fuse block contains one threadgroup.
groupshared uint uOccupied = 0;
groupshared uint uEmptyThread = 0;
[numthreads(THREAD_DIM, THREAD_DIM, THREAD_DIM)]
void main(uint3 u3GID : SV_GroupID, uint3 u3GTID : SV_GroupThreadID,
    uint uGIdx : SV_GroupIndex)
{
    uint uWorkQueueIdx = u3GID.x;
    uint3 u3BlockIdx = UnpackedToUint3(buf_srvUpdateBlockQueue[uWorkQueueIdx]);
    uint3 u3VolumeIdx = u3BlockIdx * vParam.uVoxelFuseBlockRatio + u3GTID;

    uint uBlockInfo = tex_uavFuseBlockVol[u3BlockIdx];
    bool bOccupied = uBlockInfo & BLOCKSTATEMASK_OCCUPIED;
    bool bVoxelEmpty;

    if (UpdateVoxel(u3VolumeIdx, bVoxelEmpty)) {
        if (bVoxelEmpty) {
            InterlockedAdd(uEmptyThread, 1);
        } else {
            uOccupied = 1;
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // Need only one thread in threadgroup to do the following, so early out
    if (uGIdx != 0) {
        return;
    }

    if (bOccupied && uEmptyThread == THREAD_DIM * THREAD_DIM * THREAD_DIM) {
        uBlockInfo &= ~BLOCKSTATEMASK_OCCUPIED;
        // Occupied Block is freed, add block to free block queue
        uint uFreeQueueIdx = buf_uavFreedOccupiedBlocksBuf.IncrementCounter();
        uint uOccupiedQIdx = uBlockInfo >> BLOCKSTATEMASK_IDXOFFSET;
        buf_uavFreedOccupiedBlocksBuf[uFreeQueueIdx] = uOccupiedQIdx;
        buf_uavOccupiedBlocksBuf[uOccupiedQIdx] = BLOCKFREEDMASK;
        // Update render block state
        InterlockedAdd(tex_uavRenderBlockVol[
            u3VolumeIdx / vParam.uVoxelRenderBlockRatio], -1);
    } else if (!bOccupied && uOccupied) {
        uBlockInfo |= BLOCKSTATEMASK_OCCUPIED;
        // Empty Block found surface, add block to new occupied block queue
        uint uNewQueueIdx = buf_uavNewOccupiedBlocksBuf.IncrementCounter();
        buf_uavNewOccupiedBlocksBuf[uNewQueueIdx] = PackedToUint(u3BlockIdx);
        // Update render block state
        InterlockedAdd(tex_uavRenderBlockVol[
            u3VolumeIdx / vParam.uVoxelRenderBlockRatio], 1);
    }
    // Reset block's update status for next update iteration
    tex_uavFuseBlockVol[u3BlockIdx] = uBlockInfo & ~BLOCKSTATEMASK_UPDATE;
}