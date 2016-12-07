#include "TSDFVolume_VoxelUpdate.hlsli"

#if ENABLE_BRICKS
RWTexture3D<int> tex_uavFuseBlockVol : register(u2);
groupshared uint uOccupiedTG = 0;
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
        uOccupiedTG = 1;
    }
    GroupMemoryBarrierWithGroupSync();
    if (uGIdx == 0 && uOccupiedTG) {
        tex_uavFuseBlockVol[u3DTid / vParam.uVoxelFuseBlockRatio] = 0;
    }
#endif // ENABLE_BRICKS
}