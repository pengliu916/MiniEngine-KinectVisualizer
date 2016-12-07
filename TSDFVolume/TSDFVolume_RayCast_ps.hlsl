#include "TSDFVolume.inl"
#include "TSDFVolume.hlsli"

#if TYPED_UAV
Buffer<float> tex_srvTSDFVol : register(t0);
#else // TEX3D_UAV
Texture3D<float> tex_srvTSDFVol : register(t0);
#endif
#if ENABLE_BRICKS
Texture2D<float2> tex_srvNearFar : register(t1);
Texture3D<uint> tex_srvRenderBlockVol : register(t2);
#endif // ENABLE_BRICKS
SamplerState samp_Linear : register(s0);

//------------------------------------------------------------------------------
// Structures
//------------------------------------------------------------------------------
struct Ray
{
    float3 f3o;
    float3 f3d;
};

//------------------------------------------------------------------------------
// Utility Funcs
//------------------------------------------------------------------------------
bool IntersectBox(Ray r, float3 boxmin, float3 boxmax,
    out float tnear, out float tfar)
{
    // compute intersection of ray with all six bbox planes
    float3 invR = 1.0f / r.f3d;
    float3 tbot = invR * (boxmin - r.f3o);
    float3 ttop = invR * (boxmax - r.f3o);

    // re-order intersections to find smallest and largest on each axis
    float3 tmin = min(ttop, tbot);
    float3 tmax = max(ttop, tbot);

    // find the largest tmin and the smallest tmax
    float2 t0 = max(tmin.xx, tmin.yz);
    tnear = max(t0.x, t0.y);
    t0 = min(tmax.xx, tmax.yz);
    tfar = min(t0.x, t0.y);

    return tnear <= tfar;
}

float InterpolatedRead(float3 f3Idx)
{
#if FILTER_READ == 1
    int3 i3Idx000;
    float3 f3d = modf(f3Idx - 0.5f, i3Idx000);
    float res1, res2, v1, v2;
    v1 = tex_srvTSDFVol[BUFFER_INDEX(i3Idx000 + uint3(0, 0, 0))];
    v2 = tex_srvTSDFVol[BUFFER_INDEX(i3Idx000 + uint3(1, 0, 0))];
    res1 = (1.f - f3d.x) * v1 + f3d.x * v2;
    v1 = tex_srvTSDFVol[BUFFER_INDEX(i3Idx000 + uint3(0, 1, 0))];
    v2 = tex_srvTSDFVol[BUFFER_INDEX(i3Idx000 + uint3(1, 1, 0))];
    res1 = (1.f - f3d.y) * res1 + f3d.y * ((1.f - f3d.x) * v1 + f3d.x * v2);
    v1 = tex_srvTSDFVol[BUFFER_INDEX(i3Idx000 + uint3(0, 0, 1))];
    v2 = tex_srvTSDFVol[BUFFER_INDEX(i3Idx000 + uint3(1, 0, 1))];
    res2 = (1.f - f3d.x) * v1 + f3d.x * v2;
    v1 = tex_srvTSDFVol[BUFFER_INDEX(i3Idx000 + uint3(0, 1, 1))];
    v2 = tex_srvTSDFVol[BUFFER_INDEX(i3Idx000 + uint3(1, 1, 1))];
    res2 = (1.f - f3d.y) * res2 + f3d.y * ((1.f - f3d.x) * v1 + f3d.x * v2);
    return (1.f - f3d.z) * res1 + f3d.z * res2;
#elif TEX3D_UAV && FILTER_READ > 1
    return tex_srvTSDFVol.SampleLevel(
        samp_Linear, f3Idx / vParam.u3VoxelReso, 0);
#else
    int3 i3Idx000;
    modf(f3Idx, i3Idx000);
    return tex_srvTSDFVol[BUFFER_INDEX(i3Idx000)];
#endif // !FILTER_READ
}

float UninterpolatedRead(float3 f3Idx)
{
    return tex_srvTSDFVol[BUFFER_INDEX((int3)f3Idx)];
}

float3 GetNormal(float3 f3Idx)
{
    float f000 = InterpolatedRead(f3Idx);
    float f100 = InterpolatedRead(f3Idx + float3(1.f, 0.f, 0.f));
    float f010 = InterpolatedRead(f3Idx + float3(0.f, 1.f, 0.f));
    float f001 = InterpolatedRead(f3Idx + float3(0.f, 0.f, 1.f));
    return normalize(float3(f100 - f000, f010 - f000, f001 - f000));
}

void IsoSurfaceShading(Ray eyeray, float2 f2NearFar,
    inout float4 f4OutColor, inout float fDepth)
{
    float3 f3Idx = eyeray.f3o + eyeray.f3d * f2NearFar.x;
    f3Idx = f3Idx * vParam.fInvVoxelSize + vParam.f3HalfVoxelReso;
    float t = f2NearFar.x;
    float fDeltaT = vParam.fVoxelSize;
    float3 f3IdxStep = eyeray.f3d;
    bool bSurfaceFound = false;

    int3 i3NewBlockIdx, i3BlockIdx = int3(-1, -1, -1);
    bool bActiveBlock = true;

    float3 f3PreIdx = f3Idx;
    float fPreTSDF, fCurTSDF = 1e15;

    while (t <= f2NearFar.y) {
#if ENABLE_BRICKS
        modf(f3Idx / vParam.uVoxelRenderBlockRatio, i3NewBlockIdx);
        if (any(i3BlockIdx != i3NewBlockIdx)) {
            i3BlockIdx = i3NewBlockIdx;
            bActiveBlock = tex_srvRenderBlockVol[i3BlockIdx];
//                ((tex_srvFuseBlockVol[i3BlockIdx] & BLOCKSTATEMASK_OCCUPIED)
//                    < uTGPerFuseBlock);
        }
        if (!bActiveBlock) {
            float3 f3Offset =
                i3BlockIdx * vParam.fRenderBlockSize - vParam.f3HalfVolSize;
            float2 f2BlockNearFar;
            IntersectBox(eyeray, f3Offset, f3Offset + vParam.fRenderBlockSize,
                f2BlockNearFar.x, f2BlockNearFar.y);
            t = max(t + fDeltaT, f2BlockNearFar.y + fDeltaT);
            f3Idx = eyeray.f3o + eyeray.f3d * t;
            f3Idx = f3Idx * vParam.fInvVoxelSize + vParam.f3HalfVoxelReso;
            continue;
        }
#endif
        fPreTSDF = fCurTSDF;
        fCurTSDF = UninterpolatedRead(f3Idx);
        if (fCurTSDF < 1.f) {
            fCurTSDF = InterpolatedRead(f3Idx) * vParam.fTruncDist;
        }

        if (fCurTSDF < 0) {
            bSurfaceFound = true;
            break;
        }
        f3PreIdx = f3Idx;
        f3Idx += f3IdxStep;
        t += fDeltaT;
    }

    if (!bSurfaceFound) {
        return;
    }

    float3 f3SurfPos = lerp(f3PreIdx, f3Idx, fPreTSDF / (fPreTSDF - fCurTSDF));
    f3SurfPos = (f3SurfPos - vParam.f3HalfVoxelReso) * vParam.fVoxelSize;
    float4 f4ProjPos = mul(mProjView, float4(f3SurfPos, 1.f));
    fDepth = f4ProjPos.z / f4ProjPos.w;
    float3 f3Normal = GetNormal(
        f3SurfPos * vParam.fInvVoxelSize + vParam.f3HalfVoxelReso);
    f4OutColor = float4(f3Normal * 0.5f + 0.5f, 1);
    return;
}

//------------------------------------------------------------------------------
// Pixel Shader
//------------------------------------------------------------------------------
void main(float3 f3Pos : POSITION1, float4 f4ProjPos : SV_POSITION,
    out float4 f4Col : SV_Target, out float fDepth : SV_Depth)
{
    // calculate ray intersection with bounding box
    float fTnear, fTfar;
    f4Col = float4(1.f, 1.f, 1.f, 0.f) * 0.2f;
    fDepth = 0.f;
#if ENABLE_BRICKS
    int2 uv = f4ProjPos.xy;
    float2 f2NearFar = tex_srvNearFar.Load(int3(uv, 0)).xy;
    if (f2NearFar.y >= -0.01f) {
        discard;
        return;
    }

    Ray eyeray;
    //world space
    eyeray.f3o = f4ViewPos.xyz;
    eyeray.f3d = f3Pos - eyeray.f3o;
    eyeray.f3d = normalize(eyeray.f3d);

    f2NearFar /= length(eyeray.f3d);
    fTnear = f2NearFar.x;
    fTfar = -f2NearFar.y;
    bool bHit = (fTfar - fTnear) > 0;
#else
    Ray eyeray;
    //world space
    eyeray.f3o = f4ViewPos.xyz;
    eyeray.f3d = f3Pos - eyeray.f3o;
    eyeray.f3d = normalize(eyeray.f3d);
    bool bHit =
        IntersectBox(eyeray, vParam.f3BoxMin, vParam.f3BoxMax, fTnear, fTfar);
#endif // ENABLE_BRICKS
    if (!bHit) {
        discard;
        return;
    }
    if (fTnear <= 0) {
        fTnear = 0;
    }
    IsoSurfaceShading(eyeray, float2(fTnear, fTfar), f4Col, fDepth);
    return;
}