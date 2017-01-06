#include "FastICP.inl"
#include "CalibData.inl"
Texture2D<uint> tex_srvKinectDepth : register(t0);
Texture2D<uint> tex_srvTSDFDepth : register(t1);
Texture2D<float4> tex_srvKinectNormal : register(t2);
Texture2D<float4> tex_srvTSDFNormal : register(t3);
Texture2D<float> tex_srvWeight : register(t4);

RWStructuredBuffer<float4> buf_uavData0 : register(u0);//CxCx,CxCy,CxCz,Ctr
RWStructuredBuffer<float4> buf_uavData1 : register(u1);//CxNx,CxNy,CxNz,CyCy
RWStructuredBuffer<float4> buf_uavData2 : register(u2);//CyNx,CyNy,CyNz,CyCz
RWStructuredBuffer<float4> buf_uavData3 : register(u3);//CzNx,CzNy,CzNz,CzCz
RWStructuredBuffer<float4> buf_uavData4 : register(u4);//NxNx,NxNy,NxNy,CxPQN
RWStructuredBuffer<float4> buf_uavData5 : register(u5);//NyNy,NyNz,NzNz,CyPQN
RWStructuredBuffer<float4> buf_uavData6 : register(u6);//NxPQN,NyPQN,NzPQN,CzPQN

void AllZero(uint uIdx)
{
    buf_uavData0[uIdx] = 0.f;
    buf_uavData1[uIdx] = 0.f;
    buf_uavData2[uIdx] = 0.f;
    buf_uavData3[uIdx] = 0.f;
    buf_uavData4[uIdx] = 0.f;
    buf_uavData5[uIdx] = 0.f;
    buf_uavData6[uIdx] = 0.f;
}

float3 ReprojectPt(uint2 u2xy, float fDepth)
{
    return float3(float2(u2xy - DEPTH_C) * fDepth / DEPTH_F, fDepth);
}

float GetNormalMatchedDepth(Texture2D<uint> tex_srvDepth, uint3 DTid)
{
    uint uAccDepth = tex_srvDepth.Load(DTid);
    uAccDepth += tex_srvDepth.Load(DTid, uint2(0, 1));
    uAccDepth += tex_srvDepth.Load(DTid, uint2(1, 0));
    uAccDepth += tex_srvDepth.Load(DTid, uint2(1, 1));
    return uAccDepth * -0.001f / 4.f;
}

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint uIdx = DTid.x + DTid.y * u2AlignedReso.x;
    if (tex_srvWeight.Load(DTid) < 0.05f) {
        AllZero(uIdx);
        return;
    }
    float4 f4KinectNormal = tex_srvKinectNormal.Load(DTid);
    // No valid normal data
    if (f4KinectNormal.w < 0.05f) {
        AllZero(uIdx);
        return;
    }
    float4 f4TSDFNormal = tex_srvTSDFNormal.Load(DTid);
    // No valid normal data
    if (f4TSDFNormal.w < 0.05f) {
        AllZero(uIdx);
        return;
    }
    // Normals are too different
    if (dot(f4TSDFNormal.xyz, f4KinectNormal.xyz) < fNormalDiffThreshold) {
        AllZero(uIdx);
        return;
    }
    float fDepth = GetNormalMatchedDepth(tex_srvKinectDepth, DTid);
    // p is Kinect point, q is TSDF point, n is TSDF normal
    // c = p x n
    float3 p = ReprojectPt(DTid.xy, fDepth);
    float3 n = f4TSDFNormal.xyz;
    float3 c = cross(p, n);

    float3 cc = c.xxx * c.xyz; // Get CxCx, CxCy, CxCz
    buf_uavData0[uIdx] = float4(cc, 1.f); // last element is counter

    cc = c.yyz * c.yzz; // Get CyCy, CyCz, CzCz
    float3 cn = c.x * n; // Get CxNx, CxNy, CxNz
    buf_uavData1[uIdx] = float4(cn, cc.x);

    cn = c.y * n; // Get CyNx, CyNy, CyNz
    buf_uavData2[uIdx] = float4(cn, cc.y);

    cn = c.z * n; // Get CzNx, CzNy, CzNz
    buf_uavData3[uIdx] = float4(cn, cc.z);

    fDepth = GetNormalMatchedDepth(tex_srvTSDFDepth, DTid);
    float3 q = ReprojectPt(DTid.xy, fDepth);
    float pqn = dot(p - q, n);
    float3 cpqn = c * pqn; // Get cx(p-q)n, cy(p-q)n, cz(p-q)n

    float3 nn = n.xxx * n.xyz; // Get NxNx, NxNy, NxNz
    buf_uavData4[uIdx] = float4(nn, cpqn.x);

    nn = n.yyz * n.yzz; // Get NyNy, NyNz, NzNz
    buf_uavData5[uIdx] = float4(nn, cpqn.y);

    float3 npqn = n * pqn; // Get nx(p-q)n, ny(p-q)n, nz(p-q)n
    buf_uavData6[uIdx] = float4(npqn, cpqn.z);
    return;
}