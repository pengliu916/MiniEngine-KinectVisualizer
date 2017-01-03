#if !__hlsl
#pragma once
#endif // !__hlsl

// Params
#define THREAD_X 8
#define THREAD_Y 8
#define THREAD_Z 8
#define WARP_SIZE 32
#define MAX_DEPTH 10000
#ifndef THREAD_DIM
#define THREAD_DIM 8
#endif
// Do not modify below this line

#define BLOCKSTATEMASK_OCCUPIED 0x7f
#define BLOCKSTATEMASK_UPDATE 0x80 // this will be 1 bit left to the previous
#define BLOCKSTATEMASK_IDXOFFSET 8 // total bits of the above two
#define BLOCKSTATEMASK_IDX ~0xff // 32bit mask 

#define BLOCKFREEDMASK 0x40000000

// IndirectJobParam layout:
// 0:FreeQueueStart, 4:FreeQueueCtr, 8:AddqueueStart, 12:AddqueueCtr
#define FREEQUEUE_STARTOFFSET 0
#define FREEQUEUE_JOBCOUNT 4
#define ADDQUEUE_STARTOFFSET 8
#define ADDQUEUE_JOBCOUNT 12
#define OCCUPIEDQUEUE_SIZE 16

// The length of cube triangles-strip vertices
#define CUBE_TRIANGLESTRIP_LENGTH 14
// The length of cube line-strip vertices
#define CUBE_LINESTRIP_LENGTH 19

#if __hlsl
#define CBUFFER_ALIGN
#define REGISTER(x) :register(x)
#define STRUCT(x) x
#else
#define CBUFFER_ALIGN __declspec( \
    align(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT))
#define REGISTER(x)
#define STRUCT(x) struct
typedef DirectX::XMMATRIX matrix;
typedef DirectX::XMFLOAT4 float4;
typedef DirectX::XMFLOAT3 float3;
typedef DirectX::XMFLOAT2 float2;
typedef DirectX::XMUINT3 uint3;
typedef DirectX::XMUINT2 uint2;
typedef DirectX::XMINT3 int3;
typedef DirectX::XMINT2 int2;
typedef uint32_t uint;
#endif

// will be put into constant buffer, pay attention to alignment
struct VolumeParam {
    uint3 u3VoxelReso;
    uint uVoxelFuseBlockRatio;
    uint2 u2NIU;
    uint uVoxelRenderBlockRatio;
    float fRenderBlockSize;
    float3 f3HalfVoxelReso;
    float fVoxelSize;
    int3 i3ResoVector;
    float fInvVoxelSize;
    float3 f3BoxMin;
    float fFuseBlockSize;
    float3 f3BoxMax;
    float fTruncDist;
    float3 f3HalfVolSize;
    float fMaxWeight;
};

CBUFFER_ALIGN STRUCT(cbuffer) PerFrameDataCB REGISTER(b0)
{
    // For depth sensor, doing Fusion
    matrix mDepthView;
    matrix mDepthViewInv;
    // For free virtual camera, doing visualization
    matrix mProjView;
    matrix mView;
    matrix mViewInv;
#if !__hlsl
    void* operator new(size_t i) {
        return _aligned_malloc(i,
            D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    };
    void operator delete(void* p) {
        _aligned_free(p);
    }
#endif
};

CBUFFER_ALIGN STRUCT(cbuffer) PerCallDataCB REGISTER(b1)
{
    VolumeParam vParam;
    float2 f2DepthRange;
    int2 i2DepthReso;
    int2 i2ColorReso;
    uint uTGPerFuseBlock;
    uint uTGFuseBlockRatio;
    float fWideHeightRatio;
    float fTanHFov;
    float fClipDist;
    int iDefragmentThreshold;
    matrix mXForm[3];
#if !__hlsl
    void* operator new(size_t i) {
        return _aligned_malloc(i,
            D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    };
    void operator delete(void* p) {
        _aligned_free(p);
    }
#endif
};
#undef CBUFFER_ALIGN