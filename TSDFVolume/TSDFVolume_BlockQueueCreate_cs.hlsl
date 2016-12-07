#include "TSDFVolume.inl"
#include "TSDFVolume.hlsli"
#include "CalibData.inl"

RWStructuredBuffer<uint> buf_uavUpdateBlockQueue : register(u0);
RWTexture3D<uint> tex_uavFuseBlockVol : register(u1);

void AddToUpdateQueue(uint3 u3BlockIdx)
{
    uint uWorkIdx = buf_uavUpdateBlockQueue.IncrementCounter();
    buf_uavUpdateBlockQueue[uWorkIdx] = PackedToUint(u3BlockIdx);
}

//------------------------------------------------------------------------------
// Pass_1 Update UpdateQueue from OccupiedBlock
//------------------------------------------------------------------------------
#if PASS_1
StructuredBuffer<uint> buf_srvOccupiedBlocks : register(t0);
ByteAddressBuffer buf_srvOccupiedBlocksBufCounter : register(t1);

bool IsPointVisible(float4 f4Pos)
{
    bool bResult = false;
    float4 f4Pos_c = mul(mDepthView, f4Pos);
    f4Pos_c.z *= -1.f;
    if (f4Pos_c.z > f2DepthRange.x && f4Pos_c.z < f2DepthRange.y) {
        float2 f2ImgIdx = f4Pos_c.xy * DEPTH_F / f4Pos_c.z + DEPTH_C;
        if (all(f2ImgIdx >= float2(0.f, 0.f)) && all(f2ImgIdx <= i2DepthReso)) {
            bResult = true;
        }
    }
    return bResult;
}

bool IsBlockVisible(uint3 u3BlockIdx)
{
    bool bResult = true;
    // Compiler falsely emit x4000 if I do early out return
    // 0 0 0
    float4 f4BlockCorner =
        float4(u3BlockIdx * vParam.fFuseBlockSize - vParam.f3HalfVolSize, 1.f);
    if (!IsPointVisible(f4BlockCorner)) {
        // 1 0 0
        f4BlockCorner.x += vParam.fFuseBlockSize;
        if (!IsPointVisible(f4BlockCorner)) {
            // 1 1 0
            f4BlockCorner.y += vParam.fFuseBlockSize;
            if (!IsPointVisible(f4BlockCorner)) {
                // 1 1 1
                f4BlockCorner.z += vParam.fFuseBlockSize;
                if (!IsPointVisible(f4BlockCorner)) {
                    // 0 1 1
                    f4BlockCorner.x -= vParam.fFuseBlockSize;
                    if (!IsPointVisible(f4BlockCorner)) {
                        // 0 0 1
                        f4BlockCorner.y -= vParam.fFuseBlockSize;
                        if (!IsPointVisible(f4BlockCorner)) {
                            // 1 0 1
                            f4BlockCorner.x += vParam.fFuseBlockSize;
                            if (!IsPointVisible(f4BlockCorner)) {
                                // 0 1 0
                                f4BlockCorner.x -= vParam.fFuseBlockSize;
                                f4BlockCorner.y += vParam.fFuseBlockSize;
                                f4BlockCorner.z -= vParam.fFuseBlockSize;
                                if (!IsPointVisible(f4BlockCorner)) {
                                    bResult = false;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    return bResult;
}

// Each thread test one block from OccupiedBlockBuf, and add it to
// UpdateBlockQueue if visibility test is positive
[numthreads(WARP_SIZE, 1, 1)]
void main(uint3 u3DTid : SV_DispatchThreadID)
{
    if (u3DTid.x >= buf_srvOccupiedBlocksBufCounter.Load(0) ||
        (buf_srvOccupiedBlocks[u3DTid.x] & BLOCKFREEDMASK)) {
        return;
    }
    uint3 u3BlockIdx = UnpackedToUint3(buf_srvOccupiedBlocks[u3DTid.x]);
    // Early out if block is not visible
    if (!IsBlockVisible(u3BlockIdx)) {
        return;
    }
    AddToUpdateQueue(u3BlockIdx);
    // The following also set NumEmptyTG to 0;
    tex_uavFuseBlockVol[u3BlockIdx] =
        u3DTid.x << BLOCKSTATEMASK_IDXOFFSET | BLOCKSTATEMASK_UPDATE;
}
#endif // PASS_1

//------------------------------------------------------------------------------
// Pass_2 Update UpdateQueue from DepthMap
//------------------------------------------------------------------------------
#if PASS_2
Texture2D<uint> tex_srvDepth : register(t0);

uint3 GetBlockIdx(float3 f3Pos)
{
    return uint3((f3Pos + vParam.f3HalfVolSize) / vParam.fFuseBlockSize);
}

void InterlockedAddToUpdateQueue(uint3 u3BlockIdx)
{
    uint uOrig = 1;
    InterlockedOr(
        tex_uavFuseBlockVol[u3BlockIdx], BLOCKSTATEMASK_UPDATE, uOrig);
    if ((uOrig & BLOCKSTATEMASK_UPDATE) == 0) {
        AddToUpdateQueue(u3BlockIdx);
    }
}


bool GetValidReprojectedPoint(uint2 u2Idx, out float3 f3Pos)
{
    bool bResult = false;
    f3Pos = 0.f;
    if (all(int2(u2Idx) < i2DepthReso)) {
        float z = tex_srvDepth.Load(uint3(u2Idx, 0)) / 1000.f;
        if (z >= f2DepthRange.x && z <= f2DepthRange.y) {
            float2 f2xy = (u2Idx - DEPTH_C) / DEPTH_F * z;
            f2xy.y *= -1.f;
            float4 f4Pos = float4(f2xy, -z, 1.f);
            f3Pos = mul(mDepthViewInv, f4Pos).xyz;
            // Make sure the pos is covered in the volume
            if (any(abs(f3Pos) > (vParam.f3HalfVolSize - vParam.fTruncDist))) {
                bResult = false;
            } else {
                bResult = true;
            }
        }
    }
    return bResult;
}

// Each thread process one pixel from DepthMap, and add at most 2 blocks
// into UpdateBlockQueue
[numthreads(THREAD_X, THREAD_Y, 1)]
void main(uint3 u3DTid : SV_DispatchThreadID)
{
    float3 f3Pos;
    if (!GetValidReprojectedPoint(u3DTid.xy, f3Pos)) {
        return;
    }
    float3 f3Step = vParam.fTruncDist * normalize(f3Pos - f4DepthPos.xyz);
    uint3 u3Block0Idx = GetBlockIdx(f3Pos - f3Step);
    if (!(tex_uavFuseBlockVol[u3Block0Idx] & BLOCKSTATEMASK_UPDATE)) {
        InterlockedAddToUpdateQueue(u3Block0Idx);
    }
    uint3 u3Block1Idx = GetBlockIdx(f3Pos + f3Step);
    if (any(u3Block1Idx != u3Block0Idx) &&
        !(tex_uavFuseBlockVol[u3Block1Idx] & BLOCKSTATEMASK_UPDATE)) {
        InterlockedAddToUpdateQueue(u3Block1Idx);
    }
}
#endif // PASS_2