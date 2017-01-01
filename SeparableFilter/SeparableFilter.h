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
    void OnResize(DirectX::XMUINT2 reso);
    void OnRender(GraphicsContext& gfxContext, ColorBuffer* pInputTex,
        ColorBuffer* pWeightTex);
    ColorBuffer* GetFilteredTex();

private:
    void _UpdateCB(uint2 u2Reso, float fRangeVar, int iKernelRadius,
        int iEdgeThreshold, int iEdgePixel);
    DynAlloc* _pUploadCB;
    ByteAddressBuffer _gpuCB;
    CBuffer _dataCB;
    ColorBuffer _intermediateBuf;
    ColorBuffer _filteredBuf;
    D3D12_VIEWPORT _viewport = {};
    D3D12_RECT _scisorRact = {};
};