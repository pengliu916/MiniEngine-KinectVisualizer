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
    //float t = fTime;
    //float x = 0.5f * sin(t * 0.5f) + 2.f * sin(-t);
    //float z = 0.5f * cos(t * 0.5f) + 2.f * cos(-t);
    //float y = sqrt(6.25f - x * x - z * z);
    //float3 ro = float3(x, 0.7 * y, z);
    //float3 ta = float3(0.f, 1.f, 0.f);

    //float3 f3z = normalize(ro - ta);
    //float3 f3up = float3(0.f, 1.f, 0.f);
    //float3 f3x = normalize(cross(f3up, f3z));
    //float3 f3y = normalize(cross(f3z, f3x));
    //float3x3 r = {f3x, f3y, f3z};

    //matrix m;
    //m[0] = float4(r[0], x);
    //m[1] = float4(r[1], y);
    //m[2] = float4(r[2], z);
    //m[3] = float4(0.f, 0.f, 0.f, 1.f);

    //buf_uavSensorMatrixBuf[1] = m;
    //r = transpose(r);
    //m[0] = float4(r[0], x);
    //m[1] = float4(r[1], y);
    //m[2] = float4(r[2], z);

    //buf_uavSensorMatrixBuf[0] = m;
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