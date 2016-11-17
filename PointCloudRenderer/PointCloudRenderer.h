#pragma once
class PointCloudRenderer
{
#include "PointCloudRenderer.inl"
public:
    enum RenderMode {
        kPointCloud = 0,
        kSurface = 1,
        kShadedSurface = 2,
        kNumRenderMode
    };

    PointCloudRenderer();
    ~PointCloudRenderer();
    HRESULT OnCreateResource();
    void OnDestory();
    void UpdateLightPos(const DirectX::XMFLOAT4& pos);
    void UpdateCB(const DirectX::XMFLOAT2& colReso, 
        const DirectX::XMFLOAT2& depReso, const DirectX::XMFLOAT4& colFC, 
        const DirectX::XMFLOAT4& depFC, const DirectX::XMMATRIX& dep2Col);
    void OnRender(GraphicsContext& gfxContext, const ColorBuffer* pDepthMap,
        const ColorBuffer* pColorMap, const DirectX::XMMATRIX& mProjView_T);
    void RenderGui();

private:
    RenderMode _renderMode = kPointCloud;
    CBuffer _cbData;
    GraphicsPSO _gfxRenderPSO[kNumRenderMode];
    RootSignature _rootSignature;
};