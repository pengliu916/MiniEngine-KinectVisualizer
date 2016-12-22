#pragma once
#include "ManagedBuf.h"
#include "TSDFVolume.inl"
class TSDFVolume
{
public:
    enum VolumeStruct {
        kVoxel = 0,
        kBlockVol,
        kNumStruct
    };

    enum FilterType {
        kNoFilter = 0,
        kLinearFilter,
        kSamplerLinear,
        kSamplerAniso,
        kNumFilter
    };

    // k64 : Threadgroup size is 4x4x4
    // k512: Threadgroup size is 8x8x8
    enum ThreadGroup {
        k64 = 0,
        k512,
        kTG
    };

    TSDFVolume();
    ~TSDFVolume();
    void CreateResource(const int2& depthReso, const int2& colorReso);
    void Destory();
    void ResizeVisualSurface(uint32_t width, uint32_t height);
    void ResetAllResource();
    void PreProcessing();
    void DefragmentActiveBlockQueue(ComputeContext& cptCtx);
    void UpdateVolume(ComputeContext& cptCtx, ColorBuffer* pDepthTex,
        ColorBuffer* pColorTex, const DirectX::XMMATRIX& mSensorView_T);
    void RenderSurface(GraphicsContext& gfxCtx,
        const DirectX::XMMATRIX& mVCamProj_T,
        const DirectX::XMMATRIX& mVCamView_T);
    void ExtractSurface(GraphicsContext& gfxCtx,
        const DirectX::XMMATRIX& mSensor_T);
    void RenderDebugGrid(GraphicsContext& gfxCtx, ColorBuffer* pColor,
        const DirectX::XMMATRIX& mVCamProj_t,
        const DirectX::XMMATRIX& mVCam_T);
    void RenderGui();

    ColorBuffer* GetDepthTexForProcessing();
    ColorBuffer* GetDepthTexForVisualize();

private:
    void _CreateFuseBlockVolAndRelatedBuf(
        const uint3& reso, const uint ratio);
    void _CreateRenderBlockVol(const uint3& reso, const uint ratio);
    // Data update
    void _UpdateRenderCamData(const DirectX::XMMATRIX& mVCamProjView_T,
        const DirectX::XMMATRIX& mSensorView_T);
    void _UpdateSensorData(const DirectX::XMMATRIX& mSensorView_T);
    void _UpdateVolumeSettings(const uint3 reso, const float voxelSize);
    void _UpdateBlockSettings(const uint fuseBlockVoxelRatio,
        const uint renderBlockVoxelRatio, const ThreadGroup TG);
    void _ClearBlockQueues(ComputeContext& cptCtx);
    void _CleanTSDFVols(ComputeContext& cptCtx,
        const ManagedBuf::BufInterface& buf, bool updateCB = false);
    void _CleanFuseBlockVol(ComputeContext& cptCtx, bool updateCB = false);
    void _CleanRenderBlockVol(ComputeContext& cptCtx);
    void _UpdateVolume(CommandContext& cmdCtx,
        const ManagedBuf::BufInterface& buf, ColorBuffer* pDepthTex,
        ColorBuffer* pColorTex);
    void _RenderVolume(GraphicsContext& gfxCtx,
        const ManagedBuf::BufInterface& buf, bool toOutTex = false);
    void _RenderNearFar(GraphicsContext& gfxCtx, bool toSurface = false);
    void _RenderBrickGrid(GraphicsContext& gfxCtx);
    template<class T>
    void _UpdateConstantBuffer(T& ctx);

    // Resource for extracting depthmap
    D3D12_VIEWPORT _depthViewPort = {};
    D3D12_RECT _depthSissorRect = {};
    D3D12_VIEWPORT _depthVisViewPort = {};
    D3D12_RECT _depthVisSissorRect = {};
    // Texture for the output depthmap
    ColorBuffer _depthMapProc;
    ColorBuffer _depthMapVisual;
    DepthBuffer _depthBufVisual;

    // Volume settings currently in use
    VolumeStruct _curVolStruct = kVoxel;
    FilterType _filterType = kSamplerLinear;
    uint3 _curReso;
    // new vol reso setting sent to ManagedBuf _volBuf
    uint3 _submittedReso;

    // Threadgroup size in use
    ThreadGroup _TGSize = k64;

    // per instance buffer resource
    // Texture3Ds for TSDF and its weight
    ManagedBuf _volBuf;

    // 32 bit Texture3D
    // [0] not used
    // [1] freed flag
    // [2-24] idx in occupied buf
    // [25] update flag
    // [26-31] num of empty threadgroup block
    VolumeTexture _fuseBlockVol;
    // Voxel block ratio
    int _fuseBlockVoxelRatio = 4;

    // 8 bit Texture3D for spacial structure for rendering
    VolumeTexture _renderBlockVol;
    // Voxel block ratio
    int _renderBlockVoxelRatio = 8;

    // Texture2D for ray casting range
    ColorBuffer _nearFarForVisual;
    ColorBuffer _nearFarForProcess;

    // 32bit element buffer for blocks need to be updated, and its element size
    // [0-1] not used
    // [2-11] x idx
    // [12-21] y idx
    // [22-31] z idx
    StructuredBuffer _updateBlocksBuf;
    uint32_t _updateQSize;
    
    // 32bit element buffer for allocated blocks(occupied), and its element size
    // [0] not used
    // [1] freed flag
    // [2-11] x idx
    // [12-21] y idx
    // [22-31] z idx
    StructuredBuffer _occupiedBlocksBuf;
    uint32_t _occupiedQSize;

    // 32bit element buffer for blocks need to add to _occupiedBlocksBuf
    // [0-1] not used
    // [2-11] x idx
    // [12-21] y idx
    // [22-31] z idx
    StructuredBuffer _newFuseBlocksBuf;
    uint32_t _newQSize;

    // 32bit element buffer for freed blocks in _occupiedBlocksBuf
    // [0-8] not used
    // [9-31] idx in occupied buf
    StructuredBuffer _freedFuseBlocksBuf;
    uint32_t _freedQSize;

    // Buffer for scheduling job in OccupiedQueueUpdate
    // _jobParamBuf layout:
    // 0:FreeQueueStart, 4:FreeQueueCtr, 8:AddqueueStart, 12:AddqueueCtr
    ByteAddressBuffer _jobParamBuf;

    // Buffer for indirect command
    // 0 - 11 bytes for VolumeUpdate_Pass2, X = OccupiedBlocks / WARP_SIZE
    // 12 - 23 bytes for VolumeUpdate_Pass3, X = numUpdateBlock * TG/Block
    // 24 - 35 bytes for OccupiedBlockUpdate_Pass2, X = numFreeSlots / WARP_SIZ
    // 36 - 47 bytes for OccupiedBlockUpdate_Pass3, X = numLeftOver / WARP_SIZE
    IndirectArgsBuffer _indirectParams;

    // Debug buffer for queue buffer size
    ReadBackBuffer _debugBuf;
    uint32_t* _readBackPtr;
    uint32_t _readBackData[4];
    uint64_t _readBackFence = 0;

    // GPU constant buffer for per frame update basis
    PerFrameDataCB _cbPerFrame;

    // GPU constant buffer for per call update basis
    PerCallDataCB _cbPerCall;
    // Point to vol data section in _cbPerCall
    VolumeParam* _volParam;

    // Pointers/handlers currently available
    ManagedBuf::BufInterface _curBufInterface;
};