#include "TSDFVolume.inl"
#include "TSDFVolume.hlsli"
#include "CalibData.inl"

#pragma warning(push)
// Compiler falsely emit x4000 if I do early out return
#pragma warning(disable: 4000)

RWStructuredBuffer<uint> buf_uavUpdateBlockQueue : register(u0);
RWTexture3D<uint> tex_uavFuseBlockVol : register(u1);
// Index 0: viewMatrixInv, Index 1: viewMatrix
StructuredBuffer<matrix> buf_srvSensorMatrices : register(t0);
void AddToUpdateQueue(uint3 u3BlockIdx)
{
    uint uWorkIdx = buf_uavUpdateBlockQueue.IncrementCounter();
    buf_uavUpdateBlockQueue[uWorkIdx] = PackedToUint(u3BlockIdx);
}

//------------------------------------------------------------------------------
// Pass_1 Update UpdateQueue from OccupiedBlock
//------------------------------------------------------------------------------
#if UPDATE_FROM_OCCUPIEDQ
StructuredBuffer<uint> buf_srvOccupiedBlocks : register(t1);
ByteAddressBuffer buf_srvOccupiedBlocksBufCounter : register(t2);

bool IsPointVisible(float4 f4Pos)
{
    bool bResult = false;
    float4 f4Pos_c = mul(buf_srvSensorMatrices[1], f4Pos);
    //float4 f4Pos_c = mul(mDepthView, f4Pos);
    if (f4Pos_c.z >= f2DepthRange.x || f4Pos_c.z <= f2DepthRange.y) {
        return false;
    }
    // OpenCV cam coordinate system is x right, y down, z forward while
    // I use x right, y up, neg z forward, so some axis flip is needed.
    // Multiply DEPTH_F will flip x, since f4Pos_c.z is negative, so divide
    // by that will get axis correctly converted
    float2 f2ImgIdx = f4Pos_c.xy * DEPTH_F / f4Pos_c.z + DEPTH_C;
    if (all(f2ImgIdx >= float2(0.f, 0.f)) && all(f2ImgIdx <= i2DepthReso)) {
        return true;
    }
    return false;
}

bool IsBlockVisible(uint3 u3BlockIdx)
{
    // 0 0 0
    float4 f4BlockCorner =
        float4(u3BlockIdx * vParam.fFuseBlockSize - vParam.f3HalfVolSize, 1.f);
    if (IsPointVisible(f4BlockCorner)) { return true; }
    // 1 0 0
    f4BlockCorner.x += vParam.fFuseBlockSize;
    if (IsPointVisible(f4BlockCorner)) { return true; }
    // 1 1 0
    f4BlockCorner.y += vParam.fFuseBlockSize;
    if (IsPointVisible(f4BlockCorner)) { return true; }
    // 1 1 1
    f4BlockCorner.z += vParam.fFuseBlockSize;
    if (IsPointVisible(f4BlockCorner)) { return true; }
    // 0 1 1
    f4BlockCorner.x -= vParam.fFuseBlockSize;
    if (IsPointVisible(f4BlockCorner)) { return true; }
    // 0 0 1
    f4BlockCorner.y -= vParam.fFuseBlockSize;
    if (IsPointVisible(f4BlockCorner)) { return true; }
    // 1 0 1
    f4BlockCorner.x += vParam.fFuseBlockSize;
    if (IsPointVisible(f4BlockCorner)) { return true; }
    // 0 1 0
    f4BlockCorner.x -= vParam.fFuseBlockSize;
    f4BlockCorner.y += vParam.fFuseBlockSize;
    f4BlockCorner.z -= vParam.fFuseBlockSize;
    if (IsPointVisible(f4BlockCorner)) { return true; }
    return false;
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
    tex_uavFuseBlockVol[u3BlockIdx] = u3DTid.x << BLOCKSTATEMASK_IDXOFFSET
        | BLOCKSTATEMASK_UPDATE | BLOCKSTATEMASK_OCCUPIED;
}
#endif // UPDATE_FROM_OCCUPIEDQ

//------------------------------------------------------------------------------
// Pass_2 Update UpdateQueue from DepthMap
//------------------------------------------------------------------------------
#if UPDATE_FROM_DEPTHMAP
Texture2D<float> tex_srvNormDepth : register(t1);
Texture2D<float> tex_srvWeight : register(t2);

uint3 GetBlockIdx(float3 f3Pos)
{
    return uint3((f3Pos + vParam.f3HalfVolSize) / vParam.fFuseBlockSize);
}

void InterlockedAddToUpdateQueue(uint3 u3BlockIdx)
{
    uint uOrig = BLOCKSTATEMASK_UPDATE;
    InterlockedOr(
        tex_uavFuseBlockVol[u3BlockIdx], BLOCKSTATEMASK_UPDATE, uOrig);
    if (!(uOrig & BLOCKSTATEMASK_UPDATE)) {
        AddToUpdateQueue(u3BlockIdx);
    }
}


bool GetValidReprojectedPoint(uint2 u2Idx, out float3 f3Pos)
{
    f3Pos = 0.f;
    if (any(int2(u2Idx) >= i2DepthReso) ||
        tex_srvWeight.Load(uint3(u2Idx, 0)) <= 0.05f) {
        return false;
    }
    float z = tex_srvNormDepth.Load(uint3(u2Idx.xy, 0)) * -10.f;
    if (z < f2DepthRange.x && z > f2DepthRange.y) {
        float2 f2xy = (u2Idx - DEPTH_C) / DEPTH_F * z;
        float4 f4Pos = float4(f2xy, z, 1.f);
        f3Pos = mul(buf_srvSensorMatrices[0], f4Pos).xyz;
        //f3Pos = mul(mDepthViewInv, f4Pos).xyz;
        // Make sure the pos is covered in the volume
        if (all(abs(f3Pos) < (vParam.f3HalfVolSize - vParam.fTruncDist))) {
            return true;
        }
    }
    return false;
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
    float3 f3Step =
        vParam.fTruncDist * normalize(f3Pos - buf_srvSensorMatrices[0]._m03_m13_m23);
    //  vParam.fTruncDist * normalize(f3Pos - mDepthViewInv._m03_m13_m23);
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
#endif // UPDATE_FROM_DEPTHMAP
#pragma  warning(pop)