#include "TSDFVolume.inl"
#include "TSDFVolume.hlsli"
#include "CalibData.inl"

#if FUSEDEBUG
Texture3D<uint> tex_srvFuseBlockVol : register(t1);

void main(uint uInstanceID : SV_InstanceID, in float4 f4Pos : POSITION,
    out float4 f4ProjPos : SV_POSITION, out float4 f4Col : COLOR0)
{
    uint3 u3Idx = MakeU3Idx(uInstanceID,
        vParam.u3VoxelReso / vParam.uVoxelFuseBlockRatio);
    f4ProjPos = nan();
    f4Col = nan();
    // check whether it is occupied 
    uint uState = tex_srvFuseBlockVol[u3Idx];
    bool bOccupied = uState & BLOCKSTATEMASK_OCCUPIED;
    bool bUpdateFlagged = uState & BLOCKSTATEMASK_UPDATE;
    if (!bOccupied && !bUpdateFlagged) {
        return;
    }
    float3 f3BrickOffset = u3Idx * vParam.uVoxelFuseBlockRatio *
        vParam.fVoxelSize - (vParam.u3VoxelReso >> 1) * vParam.fVoxelSize;
    float fScale = 0.7f;
    f4Col = float4(0.f, 0.f, 0.f, 1.f);
    if (bOccupied) {
        f4Col.g = 0.5f;
    }
    if (bUpdateFlagged) {
        f4Col.r = 0.9f;
        fScale = 0.2f;
    }
    f4Pos.xyz = (f4Pos.xyz * fScale + 0.5f) * vParam.fVoxelSize *
        vParam.uVoxelFuseBlockRatio + f3BrickOffset;
    f4ProjPos = mul(mProjView, f4Pos);
}

#else
Texture3D<int> tex_srvRenderBlockVol : register(t1);

void main(uint uInstanceID : SV_InstanceID, in float4 f4Pos : POSITION,
    out float4 f4ProjPos : SV_POSITION, out float2 f2Depths : NORMAL0)
{
    uint3 u3Idx = MakeU3Idx(uInstanceID,
        vParam.u3VoxelReso / vParam.uVoxelRenderBlockRatio);
    f4ProjPos = nan();
    f2Depths = nan();
    // check whether it is occupied 
    if (tex_srvRenderBlockVol[u3Idx] != 0) {
        float3 f3BrickOffset =
            u3Idx * vParam.uVoxelRenderBlockRatio * vParam.fVoxelSize -
            (vParam.u3VoxelReso >> 1) * vParam.fVoxelSize;
        f4Pos.xyz = (f4Pos.xyz + 0.5f) * vParam.fVoxelSize *
            vParam.uVoxelRenderBlockRatio + f3BrickOffset;
#if FOR_VCAMERA
        f4ProjPos = mul(mProjView, f4Pos);
        float fVecLength = length(mul(mView, f4Pos).xyz);
#endif // FOR_VCAMERA
#if FOR_SENSOR
        float4 f4Temp = mul(mDepthView, f4Pos);
        float2 f2HalfReso = i2DepthReso >> 1;
        float2 f2xy = (f4Temp.xy / f4Temp.z * DEPTH_F + DEPTH_C
            - f2HalfReso) / f2HalfReso;
        f2xy.y *= -1.f;
        f4ProjPos = float4(f2xy, 1.f, 1.f);
        float fVecLength = length(f4Temp.xyz);
#endif // FOR_SENSOR
        f2Depths = float2(fVecLength, -fVecLength);
    }
}
#endif