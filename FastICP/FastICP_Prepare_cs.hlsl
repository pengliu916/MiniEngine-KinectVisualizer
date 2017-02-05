#include "FastICP.inl"
#include "CalibData.inl"
Texture2D<float> tex_srvKinectNormDepth : register(t0);
Texture2D<float> tex_srvTSDFNormDepth : register(t1);
Texture2D<float4> tex_srvKinectNormal : register(t2);
Texture2D<float4> tex_srvTSDFNormal : register(t3);
// Confidence Texture:
// .r: related to dot(surfNor, -viewDir)
// .g: related to 1.f / dot(idx.xy, idx.xy)
// .b: related to 1.f / depth
// .a: overall confidence
Texture2D<float4> tex_srvConfidence : register(t4);

SamplerState samp_Linear : register(s0);

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

float GetNormalMatchedDepth(Texture2D<float> tex_srvNormDepth, uint3 DTid)
{
    float4 f4 = tex_srvNormDepth.Gather(
        samp_Linear, (DTid.xy + .5f) * f2InvOrigReso) * -10.f;
    return dot(f4, 1.f) * .25f;
}

[numthreads(THREAD_X, THREAD_Y, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint uIdx = DTid.x + DTid.y * u2AlignedReso.x;
    if (tex_srvConfidence.Load(DTid).a < 0.05f) {
        AllZero(uIdx);
        return;
    }
    float3 f3KinectNormal = tex_srvKinectNormal.Load(DTid).xyz * 2.f - 1.f;
    f3KinectNormal.x = dot(mXform[0].xyz, f3KinectNormal);
    f3KinectNormal.y = dot(mXform[1].xyz, f3KinectNormal);
    f3KinectNormal.z = dot(mXform[2].xyz, f3KinectNormal);

    float4 f4TSDFNormal = tex_srvTSDFNormal.Load(DTid);
    // No valid normal data
    if (f4TSDFNormal.w < 0.05f) {
        AllZero(uIdx);
        return;
    }
    // Normals are too different
    if (dot(f4TSDFNormal.xyz, f3KinectNormal) < fNormalDiffThreshold) {
        AllZero(uIdx);
        return;
    }
    float fDepth = GetNormalMatchedDepth(tex_srvKinectNormDepth, DTid);
    // p is Kinect point, q is TSDF point, n is TSDF normal
    // c = p x n
    float3 p = mul(mXform, float4(ReprojectPt(DTid.xy, fDepth), 1.f)).xyz;
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

    fDepth = GetNormalMatchedDepth(tex_srvTSDFNormDepth, DTid);
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