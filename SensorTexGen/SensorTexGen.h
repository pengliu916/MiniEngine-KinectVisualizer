#pragma once
#define VCAM_DEBUG 1
#include "IRGBDStreamer.h"
#include "LinearFrameAllocator.h"

class SensorTexGen
{
#include "SensorTexGen.inl"
public:
    enum DepthSource {
        kKinect = 0,
        kProcedual,
        kSimple,
        kNumSrcMode,
    };
    enum ProcessMode {
        kRaw = 0,
        kUndistorted = 1,
        kNumDataMode,
    };
    enum DepthMode {
        kNoDepth = -1,
        kDepth = 0,
        kDepthWithVisual = 1,
        kDepthWithVisualWithInfrared = 2,
        kNumDepthMode
    };
    enum ColorMode {
        kNoColor = -1,
        kColor = 0,
        kNumColorMode,
    };
    enum TargetTexture {
        kDepthTex = 0,
        kDepthVisualTex = 1,
        kInfraredTex = 2,
        kColorTex = 3,
        kNumTargetTex,
    };

    SensorTexGen(bool enableColor, bool enableDepth, bool enableInfrared);
    ~SensorTexGen();
    void OnCreateResource(LinearAllocator& uploadHeapAlloc);
    void OnDestory();
    // return true if get new data, false otherwise
    bool OnRender(CommandContext& cmdCtx, ColorBuffer* pDepthOut,
        ColorBuffer* pColorOut = nullptr, ColorBuffer* pInfraredOut = nullptr,
        ColorBuffer* pDepthVisOut = nullptr);
    void RenderGui();
    StructuredBuffer* GetVCamMatrixBuf();

private:
    DepthSource _depthSource = kKinect;
    ColorMode _colorMode = kColor, _preColorMode;
    DepthMode _depthMode = kDepthWithVisualWithInfrared, _preDepthMode;
    ProcessMode _processMode = kUndistorted;

    IRGBDStreamer* _pKinect2 = nullptr;
    LinearFrameAllocator* _pFrameAlloc[IRGBDStreamer::kNumBufferTypes] = {};
    KinectBuffer* _pKinectBuf[IRGBDStreamer::kNumBufferTypes] = {};
    RenderCB _cbKinect;
    DynAlloc* _pUploadCB;
    ByteAddressBuffer _gpuCB;
    StructuredBuffer _camMatrixBuf;
#if VCAM_DEBUG
    bool _showDebugData = true;
    ReadBackBuffer _debugBuf;
    uint64_t _debugFence = 0;
    float _debugData[12];
    float* _debugPtr = nullptr;
#endif

    D3D12_VIEWPORT _depthInfraredViewport = {};
    D3D12_RECT _depthInfraredScissorRect = {};
    D3D12_VIEWPORT _colorViewport = {};
    D3D12_RECT _colorScissorRect = {};

    void _RetirePreviousFrameKinectBuffer();
    bool _PrepareAndFillinKinectBuffers();
    bool _FillinKinectBuffer(
        IRGBDStreamer::BufferType, const FrameData& frame);
};
