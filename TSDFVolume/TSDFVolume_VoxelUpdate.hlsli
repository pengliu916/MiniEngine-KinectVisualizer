#include "TSDFVolume.inl"
#include "TSDFVolume.hlsli"
#include "CalibData.inl"
#pragma warning(push)
// Compiler falsely emit x4000 if I do early out return
#pragma warning(disable: 4000)
// Index 0: viewMatrixInv, Index 1: viewMatrix
StructuredBuffer<matrix> buf_srvSensorMatrices : register(t0);
Texture2D<float> tex_srvNormDepth : register(t1);
#if TYPED_UAV
RWBuffer<float> tex_uavTSDFVol : register(u0);
RWBuffer<uint> tex_uavWeightVol : register(u1);
#if NO_TYPED_LOAD
Buffer<float> tex_srvTSDFVol : register(t2);
Buffer<uint> tex_srvWeightVol : register(t3);
#endif // NO_TYPED_LOAD
#else
RWTexture3D<float> tex_uavTSDFVol : register(u0);
RWTexture3D<uint> tex_uavWeightVol : register(u1);
#if NO_TYPED_LOAD
Texture3D<float> tex_srvTSDFVol : register(t2);
Texture3D<uint> tex_srvWeightVol : register(t3);
#endif // NO_TYPED_LOAD
#endif // TYPED_UAV
// Confidence Texture:
// .r: related to dot(surfNor, -viewDir)
// .g: related to 1.f / dot(idx.xy, idx.xy)
// .b: related to 1.f / depth
// .a: overall confidence
Texture2D<float4> tex_srvConfidence : register(t4);

int2 GetProjectedUVDepth(uint3 u3DTid, out float fDepth)
{
    //float4 f4Temp = mul(mDepthView, float4(
    float4 f4Temp = mul(buf_srvSensorMatrices[1], float4(
        (u3DTid - vParam.u3VoxelReso * 0.5f + 0.5f) * vParam.fVoxelSize, 1.f));
    fDepth = f4Temp.z;
    // To match opencv camera coordinate
    int2 i2uv = f4Temp.xy / fDepth * DEPTH_F + DEPTH_C;
    return i2uv;
}

bool GetValidDepthUV(uint3 u3VolIdx, out float fDepth, out int2 i2uv)
{
    bool bResult = true;
    i2uv = GetProjectedUVDepth(u3VolIdx, fDepth);
    // Discard voxels whose projected image lay outside of depth image
    // or depth value is out of sensor range
    if (fDepth > 0.f || fDepth < f2DepthRange.y ||
        any(i2uv > i2DepthReso || i2uv < 0)) {
        bResult = false;
    }
    return bResult;
}

void FuseVoxel(BufIdx bufIdx, float fTSD, float4 f4Confidence, out bool bEmpty)
{
    // Given dir range, separate them into 4 levels: higher level means
    // SDF value is acquired by rays more perpendicular to surface, thus
    // more reliable. In this update scheme, voxel only fuse new SDF when
    // it is from at least the same level or higher
    uint uDirConfidence = f4Confidence.r * 4.f;
#if NO_TYPED_LOAD
    uint uPreConfWeight = tex_srvWeightVol[bufIdx];
    float fPreTSD = tex_srvTSDFVol[bufIdx];
#else
    uint uPreConfWeight = tex_uavWeightVol[bufIdx];
    float fPreTSD = tex_uavTSDFVol[bufIdx];
#endif
    uint uPreConfidenceLevel = uPreConfWeight >> VOLUMECONFIDENCELEVEL_OFFSET;
    uint uPreWeight = (uPreConfWeight & 0x3f);
    if (uPreConfidenceLevel > uDirConfidence) {
        bEmpty = (fPreTSD < 0.99f) ? false : true;
        return;
    }
    // The following rule will give candidate sdf weight 2 if
    // f4Confidence.a > 0.7 and weight 1 if 0.2 < f4Confidence.a < 0.7
    uint uWeight = (f4Confidence.a + 0.3) * 2;
    uint uNewWeight = uWeight + uPreWeight;
    float fTSDF = fTSD * (float)uWeight + fPreTSD * (float)uPreWeight;
    fTSDF = fTSDF / (float)uNewWeight;
    bEmpty = (fTSDF < 0.99f) ? false : true;
    tex_uavTSDFVol[bufIdx] = fTSDF;
    tex_uavWeightVol[bufIdx] = (uDirConfidence << VOLUMECONFIDENCELEVEL_OFFSET)
        | min(uNewWeight, vParam.iMaxWeight);
}

bool UpdateVoxel(uint3 u3DTid, out bool bEmpty)
{
    bEmpty = false;
    bool bResult = false;
    BufIdx bufIdx = BUFFER_INDEX(u3DTid); 
    int2 i2uv;
    float fVoxelCenterDepth;
    if (GetValidDepthUV(u3DTid, fVoxelCenterDepth, i2uv)) {
        float4 f4Confidence = tex_srvConfidence.Load(int3(i2uv, 0));
        if (f4Confidence.a < 0.2f) {
            return bResult;
        }
        float fSurfaceDepth = tex_srvNormDepth.Load(int3(i2uv, 0)) * 10.f;
        float fSD = fSurfaceDepth + fVoxelCenterDepth;
        // Discard voxels if it's far behind surface
        if (fSD > -vParam.fTruncDist) {
            float fTSD = min(fSD / vParam.fTruncDist, 1.f);
            FuseVoxel(bufIdx, fTSD, f4Confidence, bEmpty);
            bResult = true;
        }
    }
    return bResult;
}
#pragma warning(pop)