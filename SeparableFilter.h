#pragma once

class SeperableFilter
{
#include "SeparableFilter_SharedHeader.inl"
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
public:
    SeperableFilter();
    ~SeperableFilter();
    HRESULT OnCreateResoure(DXGI_FORMAT BufferFormat);
    void UpdateCB(DirectX::XMUINT2 Reso, DXGI_FORMAT BufferFormat);
    void OnRender(GraphicsContext& gfxContext, ColorBuffer* pInputTex);
    void RenderGui();

    CBuffer m_CBData;
    KernelSize m_KernelSizeInUse = k7KernelDiameter;

protected:
    GraphicsPSO m_HPassPSO[kNumKernelDiameter];
    GraphicsPSO m_VPassPSO[kNumKernelDiameter];
    RootSignature m_RootSignature;

    ColorBuffer m_WorkingBuffer;
    D3D12_VIEWPORT m_Viewport = {};
    D3D12_RECT m_ScisorRact = {};
};

