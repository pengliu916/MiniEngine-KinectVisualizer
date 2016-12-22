#include "pch.h"
#include "KinectVisualizer.h"
#include "CalibData.inl"
#include <ppl.h>

using namespace DirectX;
using namespace Microsoft::WRL;

namespace {
    DirectX::XMMATRIX _depthViewInv_T = DirectX::XMMatrixTranslation(0, 0, 3);
    bool _useBilateralFilter = false;
    bool _visualize = true;
    bool _windowActive = false;

    ImVec2 _visWinPos = ImVec2(0, 0);
    ImVec2 _visWinSize = ImVec2(1024, 768);

    struct ImGuiResizeConstrain {
        static void Step(ImGuiSizeConstraintCallbackData* data) {
            float step = (float)(int)(intptr_t)data->UserData;
            data->DesiredSize = ImVec2(
                (int)(data->DesiredSize.x / step + 0.5f) * step,
                (int)(data->DesiredSize.y / step + 0.5f) * step);
        }
    };
}

KinectVisualizer::KinectVisualizer(
    uint32_t width, uint32_t height, std::wstring name)
    : _sensorTexGen(true, true, true),
    _normalGenForOriginalDepthMap(uint2(512, 424), L"NormalForOriginalDepth"),
    _normalGenForVisualizedSurface(uint2(512, 424), L"NormalForVisSurface"),
    _normalGenForTSDFDepthMap(uint2(512, 424), L"NormalForTSDFDepth")
{
    _width = width;
    _height = height;

    auto center = XMVectorSet(0.0f, 0.0f, 0.0f, 0.0f);
    auto radius = _camOrbitRadius;
    auto minRadius = _camMinOribtRadius;
    auto maxRadius = _camMaxOribtRadius;
    auto longAngle = -DirectX::XM_PIDIV2;
    auto latAngle = DirectX::XM_PIDIV2;
    _camera.View(center, radius, minRadius, maxRadius, longAngle, latAngle);
}

KinectVisualizer::~KinectVisualizer()
{
}

void
KinectVisualizer::OnConfiguration()
{
    Core::g_config.FXAA = false;
    Core::g_config.swapChainDesc.Width = _width;
    Core::g_config.swapChainDesc.Height = _height;
    Core::g_config.swapChainDesc.BufferCount = 5;
    Core::g_config.passThroughMsg = true;
}

HRESULT
KinectVisualizer::OnCreateResource()
{
    // Create resource for pointcloudrenderer
    _sensorTexGen.OnCreateResource();
    uint16_t colWidth, colHeight, depWidth, depHeight;
    _sensorTexGen.GetColorReso(colWidth, colHeight);
    _sensorTexGen.GetDepthInfrareReso(depWidth, depHeight);

    // Create resource for BilateralFilter
    _bilateralFilter.OnCreateResoure(DXGI_FORMAT_R16_UINT);
    _bilateralFilter.UpdateCB(XMUINT2(depWidth, depHeight));

    int2 depthReso = int2(depWidth, depHeight);
    int2 colorReso = int2(colWidth, colHeight);

    _tsdfVolume.CreateResource(depthReso, colorReso);

    _normalGenForOriginalDepthMap.OnCreateResource();
    _normalGenForVisualizedSurface.OnCreateResource();
    _normalGenForTSDFDepthMap.OnCreateResource();

    OnSizeChanged();

    _ResizeVisWin();
    return S_OK;
}

HRESULT
KinectVisualizer::OnSizeChanged()
{
    return S_OK;
}

void
KinectVisualizer::OnUpdate()
{
    _windowActive = false;
    _camera.ProcessInertia();
    _tsdfVolume.PreProcessing();

    static bool showPanel = true;
    if (ImGui::Begin("KinectVisualizer", &showPanel)) {
        ImGui::Checkbox("Bilateral Filter", &_useBilateralFilter);
        if (_useBilateralFilter) {
            _bilateralFilter.RenderGui();
        }
        _tsdfVolume.RenderGui();
        _sensorTexGen.RenderGui();
        NormalGenerator::RenderGui();
    }
    ImGui::End();
    ImGui::SetNextWindowSizeConstraints(ImVec2(640,480),
        ImVec2(FLT_MAX, FLT_MAX), ImGuiResizeConstrain::Step, (void*)32);
    if (ImGui::Begin("Visualize Image")) {
        _visWinPos = ImGui::GetCursorScreenPos();
        _visWinSize = ImGui::GetContentRegionAvail();
        _visualize = true;
        if (_visWinSize.x != _width || _visWinSize.y != _height) {
            _width = static_cast<uint16_t>(_visWinSize.x);
            _height = static_cast<uint16_t>(_visWinSize.y);
            _ResizeVisWin();
        }
        ImTextureID tex_id =
            (void*)&_normalGenForVisualizedSurface.GetNormalMap()->GetSRV();
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        ImGui::InvisibleButton("canvas", _visWinSize);
        ImVec2 bbmin = _visWinPos;
        ImVec2 bbmax =
            ImVec2(_visWinPos.x + _visWinSize.x, _visWinPos.y + _visWinSize.y);
        draw_list->AddImage(tex_id, bbmin, bbmax);
        if (ImGui::IsItemHovered()) {
            _windowActive = true;
        }
    } else {
        _visualize = false;
    }
    ImGui::End();

    _normalGenForOriginalDepthMap.GetNormalMap()->GuiShow();
    _normalGenForTSDFDepthMap.GetNormalMap()->GuiShow();
}

void
KinectVisualizer::OnRender(CommandContext & cmdCtx)
{
    XMMATRIX mView_T = _camera.View();
    XMMATRIX mViewInv_T = XMMatrixInverse(nullptr, mView_T);
    XMMATRIX mProj_T = _camera.Projection();
    XMMATRIX mProjView_T = XMMatrixMultiply(mView_T, mProj_T);
    XMFLOAT4 viewPos;
    XMStoreFloat4(&viewPos, _camera.Eye());

    cmdCtx.BeginResourceTransition(Graphics::g_SceneColorBuffer,
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmdCtx.BeginResourceTransition(Graphics::g_SceneDepthBuffer,
        D3D12_RESOURCE_STATE_DEPTH_WRITE);

    GraphicsContext& gfxCtx = cmdCtx.GetGraphicsContext();
    ComputeContext& cptCtx = cmdCtx.GetComputeContext();

    // Pull new data from Kinect
    bool newData = _sensorTexGen.OnRender(cmdCtx);

    // Bilateral filtering
    if (newData && _useBilateralFilter) {
        _bilateralFilter.OnRender(gfxCtx,
            _sensorTexGen.GetOutTex(SensorTexGen::kDepthTex));
    }

    // TSDF volume updating
    _tsdfVolume.UpdateVolume(cptCtx,
        _sensorTexGen.GetOutTex(SensorTexGen::kDepthTex),
        _sensorTexGen.GetOutTex(SensorTexGen::kColorTex),
        _depthViewInv_T);

    // Defragment active block queue in TSDF
    _tsdfVolume.DefragmentActiveBlockQueue(cptCtx);

    // Request visualized surface depthmap from TSDF
    if (_visualize) {
        _tsdfVolume.RenderSurface(gfxCtx, mProj_T, mView_T);
    }
    // Request depthmap for ICP
    _tsdfVolume.ExtractSurface(gfxCtx,
        XMMatrixInverse(nullptr, _depthViewInv_T));

    // Generate normalmap for Kinect depthmap
    _normalGenForOriginalDepthMap.OnProcessing(
        cptCtx, _sensorTexGen.GetOutTex(SensorTexGen::kDepthTex));
    // Generate normalmap for TSDF depthmap
    _normalGenForTSDFDepthMap.OnProcessing(cmdCtx.GetComputeContext(),
        _tsdfVolume.GetDepthTexForProcessing());

    // Generate normalmap for visualized depthmap
    if (_visualize) {
        _normalGenForVisualizedSurface.OnProcessing(
            cptCtx, _tsdfVolume.GetDepthTexForVisualize());
        _tsdfVolume.RenderDebugGrid(gfxCtx,
            _normalGenForVisualizedSurface.GetNormalMap(), mProj_T, mView_T);
    }
}

void
KinectVisualizer::OnDestroy()
{
    _sensorTexGen.OnDestory();
    _normalGenForTSDFDepthMap.OnDestory();
    _normalGenForVisualizedSurface.OnDestory();
    _normalGenForOriginalDepthMap.OnDestory();
    _tsdfVolume.Destory();
    _bilateralFilter.OnDestory();
}

bool
KinectVisualizer::OnEvent(MSG * msg)
{
    if (!_windowActive) {
        return false;
    }
    switch (msg->message) {
    case WM_MOUSEWHEEL: {
        auto delta = GET_WHEEL_DELTA_WPARAM(msg->wParam);
        _camera.ZoomRadius(-0.01f*delta);
    }
    case WM_POINTERDOWN:
    case WM_POINTERUPDATE:
    case WM_POINTERUP: {
        auto pointerId = GET_POINTERID_WPARAM(msg->wParam);
        POINTER_INFO pointerInfo;
        if (GetPointerInfo(pointerId, &pointerInfo)) {
            if (msg->message == WM_POINTERDOWN) {
                // Camera manipulation
                _camera.AddPointer(pointerId);
            }
        }

        // Otherwise send it to the camera controls
        _camera.ProcessPointerFrames(pointerId, &pointerInfo);
        if (msg->message == WM_POINTERUP) {
            _camera.RemovePointer(pointerId);
        }
    }
    }
    return false;
}

void
KinectVisualizer::_ResizeVisWin()
{
    Graphics::g_cmdListMngr.IdleGPU();
    float fAspectRatio = _visWinSize.x / _visWinSize.y;
    _camera.Projection(XM_PIDIV2 / 2, fAspectRatio);
    uint32_t width = static_cast<uint32_t>(_visWinSize.x);
    uint32_t height = static_cast<uint32_t>(_visWinSize.y);
    _tsdfVolume.ResizeVisualSurface(width, height);
    _normalGenForVisualizedSurface.OnResize(uint2(width, height));
}