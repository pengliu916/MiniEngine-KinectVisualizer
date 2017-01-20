#include "TSDFVolume_VoxelUpdate.hlsli"

#if ENABLE_BRICKS
RWTexture3D<int> tex_uavRenderBlockVol : register(u2);
groupshared uint uOccupied = 0;
#endif // ENABLE_BRICKS
//------------------------------------------------------------------------------
// Compute Shader
//------------------------------------------------------------------------------
[numthreads(THREAD_X, THREAD_Y, THREAD_Z)]
void main(uint3 u3DTid: SV_DispatchThreadID, uint uGIdx : SV_GroupIndex)
{
    bool bEmpty = true;
    bool bUpdated = UpdateVoxel(u3DTid, bEmpty);
#if ENABLE_BRICKS
    if (bUpdated && !bEmpty) {
        uOccupied = 1;
    }
    GroupMemoryBarrierWithGroupSync();
    if (uGIdx == 0 && uOccupied) {
        tex_uavRenderBlockVol[u3DTid / vParam.uVoxelRenderBlockRatio] = 1;
    }
#endif // ENABLE_BRICKS
}