#include "TSDFVolume.inl"

// Index 0: viewMatrixInv, Index 1: viewMatrix
StructuredBuffer<matrix> buf_srvSensorMatrix : register(t0);

void
main(uint uInstanceID : SV_InstanceID, in float4 f4Pos : POSITION,
    out float4 f4ProjPos : SV_POSITION, out float4 f4Color : COLOR0)
{
    f4Pos = mul(mXForm[uInstanceID], f4Pos);
    f4Color = float4(1.0f, 0.f, 0.f, 1.f);
    if (uInstanceID > 0) {
        // The following is to expand cube into pyramid when f4Pos.w is not 1.f
        f4Pos = float4(f4Pos.xyz * -f4Pos.w, 1.f);
        f4Pos = mul(buf_srvSensorMatrix[0], f4Pos);
        //f4Pos = mul(mDepthViewInv, f4Pos);
        f4Color = float4(0.f, 1.f, 0.f, 1.f);
    }
    f4ProjPos = mul(mProjView, f4Pos);
}