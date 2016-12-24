#pragma once

class SeperableFilter
{
#include "SeparableFilter.inl"
public:
    enum KernelSize {
        k1KernelDiamiter = 0,
        k3KernelDiameter = 1,
        k5KernelDiameter = 2,
        k7KernelDiameter = 3,
        k9KernelDiameter = 4,
        k11KernelDiameter = 5,
        k13KernelDiameter = 6,
        kNumKernelDiameter
    };
    SeperableFilter();
    ~SeperableFilter();
    HRESULT OnCreateResoure(DXGI_FORMAT bufferFormat,
        LinearAllocator& uploadHeapAlloc);
    void OnDestory();
    void UpdateCB(DirectX::XMUINT2 reso);
    void OnRender(GraphicsContext& gfxContext, ColorBuffer* pInputTex);
    void RenderGui();
    ColorBuffer* GetOutTex();

private:
    DynAlloc* _pUploadCB;
    ByteAddressBuffer _gpuCB;
    CBuffer _dataCB;
    KernelSize _kernelSize = k7KernelDiameter;
    ColorBuffer _intermediateBuf;
    ColorBuffer _outBuf;
    D3D12_VIEWPORT _viewport = {};
    D3D12_RECT _scisorRact = {};
    DXGI_FORMAT _outTexFormat;
};