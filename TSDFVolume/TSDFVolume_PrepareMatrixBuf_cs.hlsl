#include "TSDFVolume.inl"
// Index 0: viewMatrixInv, Index 1: viewMatrix
RWStructuredBuffer<matrix> buf_uavSensorMatrixBuf : register(u0);
StructuredBuffer<float3> buf_srvInputMatrixBuf : register(t0);
//------------------------------------------------------------------------------
// Compute Shader
//------------------------------------------------------------------------------
[numthreads(1, 1, 1)]
void main()
{
    float3x3 r = { buf_srvInputMatrixBuf[0], buf_srvInputMatrixBuf[1],
        buf_srvInputMatrixBuf[2] };
    float3 t = buf_srvInputMatrixBuf[3];
    float3 tInv = -mul(r, t);
    matrix m;
    m[0] = float4(r[0], tInv.x);
    m[1] = float4(r[1], tInv.y);
    m[2] = float4(r[2], tInv.z);
    m[3] = float4(0.f, 0.f, 0.f, 1.f);
    buf_uavSensorMatrixBuf[1] = m;
    r = transpose(r);
    m[0] = float4(r[0], t.x);
    m[1] = float4(r[1], t.y);
    m[2] = float4(r[2], t.z);
    buf_uavSensorMatrixBuf[0] = m;
}