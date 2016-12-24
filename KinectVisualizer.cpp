#include "pch.h"
#include "KinectVisualizer.h"
#include "CalibData.inl"
#include <ppl.h>

using namespace DirectX;
using namespace Microsoft::WRL;

#define Trans(ctx, res, state) \
ctx.TransitionResource(res, state)

#define TransFlush(ctx, res, state) \
ctx.TransitionResource(res, state, true)

#define BeginTrans(ctx, res, state) \
ctx.BeginResourceTransition(res, state)

namespace {
typedef D3D12_RESOURCE_STATES State;
const State RTV   = D3D12_RESOURCE_STATE_RENDER_TARGET;
const State DSV   = D3D12_RESOURCE_STATE_DEPTH_WRITE;
const State UAV   = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
const State psSRV = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
const State csSRV = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
const State vsSRV = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

LinearAllocator _uploadHeapAlloc = {kCpuWritable};

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
    uint16_t colWidth, colHeight, depWidth, depHeight;
    _sensorTexGen.OnCreateResource(_uploadHeapAlloc);
    _sensorTexGen.GetColorReso(colWidth, colHeight);
    _sensorTexGen.GetDepthInfrareReso(depWidth, depHeight);

    // Create resource for BilateralFilter
    _bilateralFilter.OnCreateResoure(DXGI_FORMAT_R16_UINT, _uploadHeapAlloc);
    _bilateralFilter.UpdateCB(XMUINT2(depWidth, depHeight));

    int2 depthReso = int2(depWidth, depHeight);
    int2 colorReso = int2(colWidth, colHeight);

    _tsdfVolume.CreateResource(depthReso, colorReso, _uploadHeapAlloc);

    _normalGenForOriginalDepthMap.OnCreateResource(_uploadHeapAlloc);
    _normalGenForVisualizedSurface.OnCreateResource(_uploadHeapAlloc);
    _normalGenForTSDFDepthMap.OnCreateResource(_uploadHeapAlloc);

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
    ImGui::SetNextWindowSizeConstraints(ImVec2(640, 480),
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
    ColorBuffer* pRawDepth = _sensorTexGen.GetOutTex(SensorTexGen::kDepthTex);
    ColorBuffer* pRawColor = _sensorTexGen.GetOutTex(SensorTexGen::kColorTex);
    ColorBuffer* pTSDFDepth = _tsdfVolume.GetDepthTexForProcessing();
    ColorBuffer* pTSDFDepth_vis = _tsdfVolume.GetDepthTexForVisualize();
    ColorBuffer* pNormal_vis = _normalGenForVisualizedSurface.GetNormalMap();
    ColorBuffer* pFilteredDepth = _bilateralFilter.GetOutTex();
    ColorBuffer* pKinectDepth = pRawDepth;

    XMMATRIX mView_T = _camera.View();
    XMMATRIX mViewInv_T = XMMatrixInverse(nullptr, mView_T);
    XMMATRIX mProj_T = _camera.Projection();
    XMMATRIX mProjView_T = XMMatrixMultiply(mView_T, mProj_T);
    XMFLOAT4 viewPos;
    XMStoreFloat4(&viewPos, _camera.Eye());

    _tsdfVolume.PreProcessing(mProj_T, mView_T, _depthViewInv_T);

    BeginTrans(cmdCtx, Graphics::g_SceneColorBuffer, RTV);
    BeginTrans(cmdCtx, Graphics::g_SceneDepthBuffer, DSV);

    GraphicsContext& gfxCtx = cmdCtx.GetGraphicsContext();
    ComputeContext& cptCtx = cmdCtx.GetComputeContext();

    // Pull new data from Kinect
    bool newData = _sensorTexGen.OnRender(cmdCtx);

    BeginTrans(cmdCtx, *pRawDepth, psSRV | csSRV);

    // Bilateral filtering
    if (newData && _useBilateralFilter) {
        _bilateralFilter.OnRender(gfxCtx, pRawDepth);
        BeginTrans(cmdCtx, *pFilteredDepth, psSRV | csSRV);
    }

    // TSDF volume updating
    _tsdfVolume.UpdateVolume(cptCtx, pRawDepth);

    // Defragment active block queue in TSDF
    _tsdfVolume.DefragmentActiveBlockQueue(cptCtx);

    // Request visualized surface depthmap from TSDF
    UINT RT = TSDFVolume::kForProc;
    if (_visualize) {
        RT = (RT | TSDFVolume::kForVisu);
    }
    // Request depthmap for ICP
    _tsdfVolume.ExtractSurface(gfxCtx, (TSDFVolume::OutSurf)RT);
    BeginTrans(gfxCtx, *pTSDFDepth, csSRV);
    if (_visualize) {
        BeginTrans(gfxCtx, *pTSDFDepth_vis, csSRV);
    }

    // Generate normalmap for visualized depthmap
    if (_visualize) {
        _normalGenForVisualizedSurface.OnProcessing(cptCtx, pTSDFDepth_vis);
        _tsdfVolume.RenderDebugGrid(gfxCtx, pNormal_vis);
        BeginTrans(gfxCtx, *pNormal_vis, psSRV);
    }
    // Generate normalmap for TSDF depthmap
    _normalGenForTSDFDepthMap.OnProcessing(cptCtx, pTSDFDepth);
    // Generate normalmap for Kinect depthmap
    if (_useBilateralFilter) {
        pKinectDepth = pFilteredDepth;
    }
    _normalGenForOriginalDepthMap.OnProcessing(cptCtx, pKinectDepth);
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