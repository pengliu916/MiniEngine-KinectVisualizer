#pragma once

class SeperableFilter
{
#include "SeparableFilter.inl"
public:
    static void RenderGui();

    SeperableFilter();
    ~SeperableFilter();
    HRESULT OnCreateResoure(LinearAllocator& uploadHeapAlloc);
    void OnDestory();
    void OnRender(GraphicsContext& gfxContext, const std::wstring procName,
        ColorBuffer* pInputTex, ColorBuffer* pOutputTex,
        ColorBuffer* pWeightTex);
    bool IsEnabled();

private:
    void _Resize(DirectX::XMUINT2 reso);
    void _UpdateCB(uint2 u2Reso, float fRangeVar, int iKernelRadius,
        float fEdgeThreshold, int iEdgePixel);
    DynAlloc* _pUploadCB;
    ByteAddressBuffer _gpuCB;
    CBuffer _dataCB;
    ColorBuffer _intermediateBuf;
    D3D12_VIEWPORT _viewport = {};
    D3D12_RECT _scisorRact = {};
};