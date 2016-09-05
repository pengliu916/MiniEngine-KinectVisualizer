#pragma once
#include "IRGBDStreamer.h"
#include "LinearFrameAllocator.h"

class SensorTexGen
{
#include "SensorTexGen_SharedHeader.inl"
public:
    enum ProcessMode {
        kRaw = 0,
        kUndistorted = 1,
        kNumDataMode
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
    std::string TargetTexName[kNumTargetTex] = 
        {"Depth Raw","Depth Visualized","Infrared Gamma","Color Raw"};

    SensorTexGen(bool EnableColor, bool EnableDepth, bool EnableInfrared);
    ~SensorTexGen();
    void OnCreateResource();
    void OnDestory();
    void OnRender(CommandContext& EngineContext);
    void RenderGui();
    void GetDepthInfrareReso(uint16_t& width, uint16_t& height);
    void GetColorReso(uint16_t& width, uint16_t& height);

    ColorMode m_ColorMode = kColor;
    DepthMode m_DepthMode = kDepthWithVisualWithInfrared;
    ProcessMode m_ProcessMode = kUndistorted;
    bool m_UseCS = true;
    bool m_Streaming = true;

    ColorBuffer m_OutTexture[kNumTargetTex];
    D3D12_CPU_DESCRIPTOR_HANDLE m_OutTextureRTV[kNumTargetTex];

protected:

    IRGBDStreamer* m_pKinect2;
    LinearFrameAllocator* m_pFrameAlloc[IRGBDStreamer::kNumBufferTypes];
    KinectBuffer* m_pKinectBuffer[IRGBDStreamer::kNumBufferTypes];
    RenderCB m_KinectCB;

    RootSignature m_RootSignature;
    GraphicsPSO m_GfxDepthPSO[kNumDepthMode][kNumDataMode];
    GraphicsPSO m_GfxColorPSO[kNumColorMode][kNumDataMode];
    ComputePSO m_CptDepthPSO[kNumDepthMode][kNumDataMode];
    ComputePSO m_CptColorPSO[kNumColorMode][kNumDataMode];
    D3D12_VIEWPORT m_DepthInfraredViewport = {};
    D3D12_RECT m_DepthInfraredScissorRect = {};
    D3D12_VIEWPORT m_ColorViewport = {};
    D3D12_RECT m_ColorScissorRect = {};
};
