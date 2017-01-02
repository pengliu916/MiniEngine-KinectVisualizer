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

ColorBuffer _depthTex_Kinect;
ColorBuffer _colorTex_Kinect;
ColorBuffer _infraredTex_Kinect;
ColorBuffer _visDepTex_Kinect;

ColorBuffer _weightTex;
ColorBuffer _normalTex_rawDepth;
ColorBuffer _normalTex_TSDFDepth;
ColorBuffer _normalTex_visualize;
ColorBuffer _depthTex_rawFiltered;
ColorBuffer _depthTex_TSDF;
ColorBuffer _depthTex_TSDFVis;
DepthBuffer _depthBuf_TSDFVis;

DirectX::XMMATRIX _depthViewInv_T = DirectX::XMMatrixTranslation(0, 0, 3);
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

KinectVisualizer::KinectVisualizer(uint32_t width, uint32_t height)
    : _sensorTexGen(true, true, true)
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
    Core::g_config.useSceneBuf = false;
}

HRESULT
KinectVisualizer::OnCreateResource()
{
    _depthTex_Kinect.Create(L"KinectDepth",
        DEPTH_RESO.x, DEPTH_RESO.y, 1, DXGI_FORMAT_R16_UINT);
    _colorTex_Kinect.Create(L"KinectColor",
        COLOR_RESO.x, COLOR_RESO.y, 1, DXGI_FORMAT_R11G11B10_FLOAT);
    _infraredTex_Kinect.Create(L"KinectInfrared",
        DEPTH_RESO.x, DEPTH_RESO.y, 1, DXGI_FORMAT_R11G11B10_FLOAT);
    _visDepTex_Kinect.Create(L"KinectDepthVis",
        DEPTH_RESO.x, DEPTH_RESO.y, 1, DXGI_FORMAT_R11G11B10_FLOAT);

    _weightTex.Create(L"WeightOut",
        DEPTH_RESO.x, DEPTH_RESO.y, 1, DXGI_FORMAT_R8_UNORM);
    _normalTex_rawDepth.Create(L"RawNorm",
        DEPTH_RESO.x, DEPTH_RESO.y, 1, DXGI_FORMAT_R10G10B10A2_UNORM);
    _normalTex_TSDFDepth.Create(L"TSDFNor",
        DEPTH_RESO.x, DEPTH_RESO.y, 1, DXGI_FORMAT_R10G10B10A2_UNORM);
    _depthTex_rawFiltered.Create(L"RawFiltered",
        DEPTH_RESO.x, DEPTH_RESO.y, 1, DXGI_FORMAT_R16_UINT);
    _depthTex_TSDF.Create(L"TSDFDepth",
        DEPTH_RESO.x, DEPTH_RESO.y, 1, DXGI_FORMAT_R16_UINT);

    _sensorTexGen.OnCreateResource(_uploadHeapAlloc);

    // Create resource for BilateralFilter
    _bilateralFilter.OnCreateResoure(_uploadHeapAlloc);

    _tsdfVolume.CreateResource(DEPTH_RESO, _uploadHeapAlloc);

    _normalGen.OnCreateResource(_uploadHeapAlloc);

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
        _sensorTexGen.RenderGui();
        SeperableFilter::RenderGui();
        NormalGenerator::RenderGui();
        _tsdfVolume.RenderGui();
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
            (void*)&_normalTex_visualize.GetSRV();
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

    _normalTex_rawDepth.GuiShow();
    _normalTex_TSDFDepth.GuiShow();
    _weightTex.GuiShow();
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

    _tsdfVolume.PreProcessing(mProj_T, mView_T, _depthViewInv_T);

    GraphicsContext& gfxCtx = cmdCtx.GetGraphicsContext();
    ComputeContext& cptCtx = cmdCtx.GetComputeContext();
    static FLOAT ClearVal[4] = {1.f, 1.f, 1.f, 1.f};
    Trans(cptCtx, _weightTex, UAV);
    cptCtx.FlushResourceBarriers();
    cptCtx.ClearUAV(_weightTex, ClearVal);
    BeginTrans(cptCtx, _weightTex, UAV);
    BeginTrans(cptCtx, _depthTex_rawFiltered, RTV);
    BeginTrans(cptCtx, Graphics::g_SceneDepthBuffer, DSV);

    // Pull new data from Kinect
    bool newData = _sensorTexGen.OnRender(cmdCtx, &_depthTex_Kinect,
        &_colorTex_Kinect, &_infraredTex_Kinect, &_visDepTex_Kinect);

    // Bilateral filtering
    _bilateralFilter.OnRender(gfxCtx, L"Filter_Raw",
        &_depthTex_Kinect, &_depthTex_rawFiltered, &_weightTex);

    BeginTrans(cptCtx, _weightTex, csSRV);
    if (_bilateralFilter.IsEnabled()) {
        BeginTrans(cptCtx, _depthTex_rawFiltered, csSRV);
    }
    // TSDF volume updating
    _tsdfVolume.UpdateVolume(cptCtx, &_depthTex_Kinect, &_weightTex);

    BeginTrans(cptCtx, _weightTex, UAV);
    // Defragment active block queue in TSDF
    _tsdfVolume.DefragmentActiveBlockQueue(cptCtx);

    // Request depthmap for ICP
    _tsdfVolume.ExtractSurface(gfxCtx, &_depthTex_TSDF,
        _visualize ? &_depthTex_TSDFVis : nullptr, &_depthBuf_TSDFVis);
    if (_visualize) {
        BeginTrans(gfxCtx, _depthTex_TSDFVis, csSRV);
    }

    // Generate normalmap for TSDF depthmap
    _normalGen.OnProcessing(cptCtx, L"Norm_TSDF",
        &_depthTex_TSDF, &_normalTex_TSDFDepth);
    // Generate normalmap for Kinect depthmap
    _normalGen.OnProcessing(cptCtx, L"Norm_Raw",
        _bilateralFilter.IsEnabled() ? &_depthTex_rawFiltered
                                     : &_depthTex_Kinect,
        &_normalTex_rawDepth, &_weightTex);
    // Generate normalmap for visualized depthmap
    if (_visualize) {
        _normalGen.OnProcessing(cptCtx, L"Norm_Vis",
            &_depthTex_TSDFVis, &_normalTex_visualize);
        _tsdfVolume.RenderDebugGrid(
            gfxCtx, &_normalTex_visualize, &_depthBuf_TSDFVis);
        Trans(gfxCtx, _normalTex_visualize, psSRV);
    }
    BeginTrans(cptCtx, _weightTex, UAV);
}

void
KinectVisualizer::OnDestroy()
{
    _depthTex_Kinect.Destroy();
    _colorTex_Kinect.Destroy();
    _infraredTex_Kinect.Destroy();
    _visDepTex_Kinect.Destroy();
    _depthBuf_TSDFVis.Destroy();
    _depthTex_TSDFVis.Destroy();
    _depthTex_TSDF.Destroy();
    _weightTex.Destroy();
    _depthTex_rawFiltered.Destroy();
    _normalTex_rawDepth.Destroy();
    _normalTex_TSDFDepth.Destroy();
    _normalTex_visualize.Destroy();
    _sensorTexGen.OnDestory();
    _normalGen.OnDestory();
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

    _depthTex_TSDFVis.Destroy();
    _depthTex_TSDFVis.Create(L"TSDFDepthVis",
        width, height, 1, DXGI_FORMAT_R16_UINT);
    _depthBuf_TSDFVis.Destroy();
    _depthBuf_TSDFVis.Create(L"TSDFDepthVisDepth",
        width, height, 1, Graphics::g_SceneDepthBuffer.GetFormat());
    _normalTex_visualize.Destroy();
    _normalTex_visualize.Create(L"VisNorm",
        width, height, 1, DXGI_FORMAT_R10G10B10A2_UNORM);
}