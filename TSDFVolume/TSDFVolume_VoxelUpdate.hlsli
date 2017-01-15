#include "TSDFVolume.inl"
#include "TSDFVolume.hlsli"
#include "CalibData.inl"

Texture2D<float> tex_srvNormDepth : register(t0);
#if TYPED_UAV
RWBuffer<float> tex_uavTSDFVol : register(u0);
RWBuffer<float> tex_uavWeightVol : register(u1);
#if NO_TYPED_LOAD
Buffer<float> tex_srvTSDFVol : register(t1);
Buffer<float> tex_srvWeightVol : register(t2);
#endif // NO_TYPED_LOAD
#endif // TYPED_UAV
#if TEX3D_UAV
RWTexture3D<float> tex_uavTSDFVol : register(u0);
RWTexture3D<uint> tex_uavWeightVol : register(u1);
#if NO_TYPED_LOAD
Texture3D<float> tex_srvTSDFVol : register(t1);
Texture3D<uint> tex_srvWeightVol : register(t2);
#endif // NO_TYPED_LOAD
#endif // TEX3D_UAV

int2 GetProjectedUVDepth(uint3 u3DTid, out float fDepth)
{
    float4 f4Temp = mul(mDepthView, float4(
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

void FuseVoxel(BufIdx bufIdx, float fTSD, out bool bEmpty)
{
#if NO_TYPED_LOAD
    float fPreWeight = (float)tex_srvWeightVol[bufIdx];
    float fPreTSD = tex_srvTSDFVol[bufIdx];
#else
    float fPreWeight = (float)tex_uavWeightVol[bufIdx];
    float fPreTSD = tex_uavTSDFVol[bufIdx];
#endif
    float fNewWeight = 1.f + fPreWeight;
    float fTSDF = fTSD + fPreTSD * fPreWeight;
    fTSDF = fTSDF / fNewWeight;
    bEmpty = (fTSDF < 0.99f) ? false : true;
    tex_uavTSDFVol[bufIdx] = fTSDF;
    tex_uavWeightVol[bufIdx] = (uint)min(fNewWeight, vParam.fMaxWeight);
}

bool UpdateVoxel(uint3 u3DTid, out bool bEmpty)
{
    bEmpty = false;
    bool bResult = false;
    BufIdx bufIdx = BUFFER_INDEX(u3DTid);
    int2 i2uv;
    float fDepthToSensor;
    if (GetValidDepthUV(u3DTid, fDepthToSensor, i2uv)) {
        float fSurfaceDepthToSensor = tex_srvNormDepth.Load(int3(i2uv, 0)) * 10.f;
        float fSD = fSurfaceDepthToSensor + fDepthToSensor;
        // Discard voxels if it's far behind surface
        if (fSD > -vParam.fTruncDist) {
            float fTSD = min(fSD / vParam.fTruncDist, 1.f);
            FuseVoxel(bufIdx, fTSD, bEmpty);
            bResult = true;
        }
    }
    return bResult;
}