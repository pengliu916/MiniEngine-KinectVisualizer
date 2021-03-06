float opUnion(float fDist1, float fDist2)
{
    return (fDist1 < fDist2) ? fDist1 : fDist2;
}
//------------------------------------------------------------------

float sdPlane(float3 f3Pos)
{
    return f3Pos.y;
}

float sdSphere(float3 f3Pos, float fRadius)
{
    return length(f3Pos) - fRadius;
}

float sdBox(float3 f3Pos, float3 f3HalfSize)
{
    float3 f3d = abs(f3Pos) - f3HalfSize;
    return length(max(f3d, 0.f));
}

float sdHexPrism(float3 f3Pos, float2 f2H)
{
    float3 q = abs(f3Pos);
    return max(q.z - f2H.y, max((q.x * 0.866025f + q.y * 0.5f), q.y) - f2H.x);
}

float sdPryamid4(float3 f3Pos, float3 f3H) // h = { cos a, sin a, height }
{
    // Tetrahedron = Octahedron - Cube
    float box = sdBox(f3Pos - float3(0.f, -2.f * f3H.z, 0.f), 2.f * f3H.z);
    float d = 0.f;
    d = max(d, abs(dot(f3Pos, float3(-f3H.x, f3H.y, 0.f))));
    d = max(d, abs(dot(f3Pos, float3(f3H.x, f3H.y, 0.f))));
    d = max(d, abs(dot(f3Pos, float3(0.f, f3H.y, f3H.x))));
    d = max(d, abs(dot(f3Pos, float3(0.f, f3H.y, -f3H.x))));
    float octa = d - f3H.z;
    return max(-box, octa); // Subtraction
}

float udCross(float3 f3Pos, float3 f3Param)
{
    float dist = opUnion(sdBox(f3Pos, f3Param), sdBox(f3Pos, f3Param.yzx));
    return opUnion(dist, sdBox(f3Pos, f3Param.zxy));
}
//------------------------------------------------------------------

float map(float3 f3Pos)
{
    float3 offset = float3(0.f, 0.75f, 0.f);
    float3 scale = float3(0.1f, 0.75f, 0.1f);
    float dist = opUnion(sdPlane(f3Pos), udCross(f3Pos - offset, scale));
    offset = float3(1.f, 0.25f, 1.f);
    scale = float3(0.25f, 0.25f, 0.25f);
    dist = opUnion(dist, sdBox(f3Pos - offset, scale));
    offset = float3(-1.f, 0.25f, -1.f);
    dist = opUnion(dist, sdSphere(f3Pos - offset, 0.25f));
    offset = float3(1.f, 0.5f, -1.f);
    dist = opUnion(dist, sdPryamid4(f3Pos - offset, float3(0.8f, 0.6f, 0.25f)));
    offset = float3(-1.f, 0.5f, 1.f);
    dist = opUnion(dist, sdHexPrism(f3Pos - offset, float2(0.25f, 0.05f)));
    return dist;
}

float castRay(float3 f3RayOrig, float3 f3RayDir)
{
    float tmin = .2f;
    float tmax = 10.f;
    float t = tmin;
    for (int i = 0; i < 64; i++) {
        float precis = 0.0005f * t;
        float res = map(f3RayOrig + f3RayDir * t);
        if (res < precis || t > tmax) break;
        t += res;
    }
    return t;
}

float3 calcNormal(float3 f3Pos)
{
    float2 e = float2(1.0f, -1.0f) * 0.5773f * 0.0005f;
    return normalize(e.xyy * map(f3Pos + e.xyy) + e.yyx * map(f3Pos + e.yyx) +
        e.yxy * map(f3Pos + e.yxy) + e.xxx * map(f3Pos + e.xxx));
    /*
    float3 eps = float3( 0.0005, 0.0, 0.0 );
    float3 nor = float3(
    map(pos+eps.xyy).x - map(pos-eps.xyy).x,
    map(pos+eps.yxy).x - map(pos-eps.yxy).x,
    map(pos+eps.yyx).x - map(pos-eps.yyx).x );
    return normalize(nor);
    */
}

float3 render(float3 f3RayOrig, float3 f3RayDir)
{
    float t = castRay(f3RayOrig, f3RayDir);
    if (t > 10.f) t = 0.f;
    float3 pos = f3RayOrig + t * f3RayDir;
    //float3 nor = calcNormal(pos);
    return pos;
}

float3x3 setCamera(float3 f3RayOrig, float3 f3Target, float fUpParam)
{
    // Camera is looking along -z axis so f3RayOrig - f3Target
    float3 f3z = normalize(f3RayOrig - f3Target);
    float3 f3up = float3(sin(fUpParam), cos(fUpParam), 0.f);
    float3 f3x = normalize(cross(f3up, f3z));
    float3 f3y = normalize(cross(f3z, f3x));
    // Row1: f3x, Row2: f3y, Row3: f3z
    return float3x3(f3x, f3y, f3z);
}

float4 GetBilinearWeights(float2 f2uv)
{
    float4 f4w;
    float2 f2uv_1 = 1.f - f2uv;
    f4w.x = f2uv_1.x * f2uv_1.y;
    f4w.y = f2uv.x * f2uv_1.y;
    f4w.z = f2uv.y * f2uv_1.x;
    f4w.w = f2uv.x * f2uv.y;
    return f4w;
}

float2 correctDistortion(float2 c, float2 f, float2 p, float4 k, uint2 idx)
{
    // For opencv r is distance from 0,0 to X/Z,Y/Z 
    float2 xy = (idx - c) / f;
    float r2 = dot(xy, xy);
    float r4 = r2 * r2;
    float r6 = r2 * r4;
    // For radial and tangential distortion correction
    float2 new_xy =
        xy * (1.f + k.x * r2 + k.y * r4 + k.z * r6) +
        2.f * p * xy.x * xy.y + p.yx * (r2 + 2.f * xy * xy);
    return new_xy * f + c;
}

#if COLOR_TEX
static const float2 f2c = COLOR_C;
static const float2 f2f = COLOR_F;
static const float2 f2p = COLOR_P;
static const float4 f4k = COLOR_K;
#define U2RESO u2ColorReso
Buffer<uint4> ColBuffer : register(t2);

float4 GetColor(uint2 u2Out_xy, uint uId, float4 f4w)
{
#   if UNDISTORTION
    float4 f400 = ColBuffer[uId];
    float4 f410 = ColBuffer[uId + 1];
    float4 f401 = ColBuffer[uId + U2RESO.x];
    float4 f411 = ColBuffer[uId + U2RESO.x + 1];
    return (f400 * f4w.x + f410 * f4w.y + f401 * f4w.z + f411 * f4w.w) / 255.f;
#   else
    return ColBuffer[uId] / 255.f;
#   endif // UNDISTORTION
}
#endif // COLOR_TEX

#if DEPTH_TEX || INFRARED_TEX || VISUALIZED_DEPTH_TEX
static const float2 f2c = DEPTH_C;
static const float2 f2f = DEPTH_F;
static const float2 f2p = DEPTH_P;
static const float4 f4k = DEPTH_K;
#define U2RESO u2DepthInfraredReso
Buffer<uint> DepBuffer : register(t0);

#   if DEPTH_SOURCE != 0 // !kKinect
RWStructuredBuffer<float3> camMatrixBuf : register(u4);

float GetFakedNormDepth(uint2 u2uv)
{
    float2 f2ab = (u2uv - f2c) / f2f;
    f2ab.xy *= -1.f;
    float t = fTime;

#       if DEPTH_SOURCE == 1 // kProcedual
    // camera
    // the trajectory is demonstrated in here
    // https://www.desmos.com/calculator/zrv7kxlhvc
    float x = (0.5f * sin(t * 0.5f) + 2.f * sin(-t)) * 1.5f;
    float z = (0.5f * cos(t * 0.5f) + 2.f * cos(-t)) * 1.5f;
    float y = sqrt(14.0625f - x * x - z * z);
    float3 ro = float3(x, y, z);
    float3 ta = float3(0.f, 1.f, 0.f);
    // camera-to-world transformation
    float3x3 ca = setCamera(ro, ta, 0.f);
    // Arbitrary chosen thread to write to that buffer
    if (all(u2uv == uint2(50, 50))) {
        // Each row is the base for camera matrix (mRViewInv)
        camMatrixBuf[0] = ca[0];
        camMatrixBuf[1] = ca[1];
        camMatrixBuf[2] = ca[2];
        camMatrixBuf[3] = float3(x, y ,z);
    }
    float3 rd = mul(transpose(ca), normalize(float3(f2ab, -1.f)));

    float3 pos = render(ro, rd);
    float fDepth = dot(pos - ro, normalize(ta - ro));
    return fDepth > .2f ? fDepth * 0.1f : 0.f;
#       endif // kProcedual

#       if DEPTH_SOURCE == 2 // kSimple
    float x = sin(t * 0.5f) * 0.8f;
    float y = cos(t * 0.5f) * 0.8f;
    float z = fFgDist;
    float4 f4S = float4(x, y, z, 0.5f);
    float fResult = fBgDist;
    float fA = dot(f2ab, f2ab) + 1.f;
    float fB = -2.f * (dot(f2ab, f4S.xy) + f4S.z);
    float fC = dot(f4S.xyz, f4S.xyz) - f4S.w * f4S.w;
    float fB2M4AC = fB * fB - 4.f * fA * fC;
    if (fB2M4AC >= 0.f) {
        fResult = min((-fB - sqrt(fB2M4AC)) / (2.f * fA), fBgDist);
    }
    if (any(u2uv == uint2(12, 12)) || any(u2uv == uint2(500, 412)) ||
        any(u2uv == uint2(256, 212))) {
        fResult += .2f;
    }
    return fResult * 0.1f;
#       endif // kSimple
}
#   endif // !kKinect

float GetNormDepth(uint2 u2Out_xy, uint uId, float4 f4w)
{
#   if DEPTH_SOURCE != 0 // !kKinect
    return GetFakedNormDepth(u2Out_xy);
#   else
#       if UNDISTORTION
    uint4 u4;
    u4.x = DepBuffer[uId];
    u4.y = DepBuffer[uId + 1];
    u4.z = DepBuffer[uId + U2RESO.x];
    u4.w = DepBuffer[uId + U2RESO.x + 1];
    float4 f4 = (u4 > 200 && u4 < 10000);
    // * 0.001 to get meter, and since range [0,10] meters need to map to [0, 1]
    // * 0.1 again
    return (dot(u4 * f4, f4w) / dot(f4, f4w) + 0.5f) * 0.0001f;
#       else
    return DepBuffer[uId] * 0.0001f;
#       endif // UNDISTORTION
#   endif // !kKinect
}
#   if INFRARED_TEX
Buffer<uint> InfBuffer : register(t1);
float GetInfrared(uint2 u2Out_xy, uint uId, float4 f4w)
{
#       if UNDISTORTION
    uint4 u4;
    u4.x = InfBuffer[uId];
    u4.y = InfBuffer[uId + 1];
    u4.z = InfBuffer[uId + U2RESO.x];
    u4.w = InfBuffer[uId + U2RESO.x + 1];
#pragma warning(push)
    // dot(u4, f4w) guarantee to return positive so suppress the warning
#pragma warning(disable: 3571)
    return pow(dot(u4, f4w) / 65535.f, 0.32f);
#pragma warning(pop)
#       else
    return pow(InfBuffer[uId] / 65535.f, 0.32f);
#       endif // UNDISTORTION
}
#   endif // INFRARED_TEX
#endif // DEPTH_TEX || INFRARED_TEX || VISUALIZED_DEPTH_TEX