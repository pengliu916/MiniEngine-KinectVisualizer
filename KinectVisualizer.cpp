#include "pch.h"
#include "KinectVisualizer.h"
#include "CalibData.inl"
#include "Reduction/Reduction.h"
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
bool _visualize = true;
bool _windowActive = false;

ImVec2 _visWinPos = ImVec2(0, 0);
ImVec2 _visWinSize = ImVec2(1024, 768);

enum ViewSize {
    FIXED_SIZE = 0,
    VARI_SIZE = 1,
    COLOR_SIZE = (1 << 1) | FIXED_SIZE,
    DEPTH_SIZE = (1 << 2) | FIXED_SIZE,
    VARI_SIZE1 = (1 << 3) | VARI_SIZE,
};

struct SurfBuffer {
    ViewSize sizeCode;
    DXGI_FORMAT colorFormat;
    DXGI_FORMAT depthFormat;
    ColorBuffer* colBuf;
    DepthBuffer* depBuf;
};

enum SurfBufId : uint8_t {
#define DEF_SURFBUF(_name, _size, _colformat, _depthformat) _name,
#include "surfbuf_defs.h"
#undef DEF_SURFBUF
    SURFBUF_COUNT
};

const wchar_t* _bufNames[] = {
#define DEF_SURFBUF(_name, _size, _colformat, _depthformat) L"SURFBUF_" #_name,
#include "surfbuf_defs.h"
#undef DEF_SURFBUF
};
CASSERT(ARRAY_COUNT(_bufNames) == SURFBUF_COUNT);

SurfBuffer _surfBufs[] = {
#define DEF_SURFBUF(_name, _size, _colformat, _depthformat) \
{_size, _colformat, _depthformat, nullptr, nullptr},
#include "surfbuf_defs.h"
#undef DEF_SURFBUF
};
CASSERT(ARRAY_COUNT(_surfBufs) == SURFBUF_COUNT);

bool _sufDebugShow[ARRAY_COUNT(_surfBufs)] = {};
std::vector<std::pair<uint8_t, bool>> _surfDebugShow;

uint2 GetSize(const ViewSize& sizeCode) {
    switch (sizeCode)
    {
    case COLOR_SIZE:
        return COLOR_RESO;
    case DEPTH_SIZE:
        return DEPTH_RESO;
    case VARI_SIZE1:
        return uint2((uint32_t)_visWinSize.x, (uint32_t)_visWinSize.y);
    }
    return uint2(0, 0);
}

void CreatBufResource(SurfBufId id) {
    SurfBuffer& item = _surfBufs[id];
    uint2 reso = GetSize(_surfBufs[id].sizeCode);
    item.colBuf->Destroy();
    item.colBuf->Create(_bufNames[id], reso.x, reso.y, 1, item.colorFormat);
    if (item.depthFormat != DXGI_FORMAT_UNKNOWN) {
        item.depBuf->Destroy();
        wchar_t temp[32];
        swprintf_s(temp, 32, L"%ls_DEP", _bufNames[id]);
        item.depBuf->Create(temp, reso.x, reso.y, item.depthFormat);
    }
}

void InitialSurfBuffer(SurfBufId id) {
    _surfBufs[id].colBuf = new ColorBuffer();
    _surfBufs[id].depBuf = new DepthBuffer();
    if (_surfBufs[id].colorFormat == DXGI_FORMAT_R11G11B10_FLOAT ||
        _surfBufs[id].colorFormat == DXGI_FORMAT_R8_UNORM ||
        _surfBufs[id].colorFormat == DXGI_FORMAT_R16_UNORM ||
        _surfBufs[id].colorFormat == DXGI_FORMAT_R10G10B10A2_UNORM) {
        _surfDebugShow.push_back({id, false});
    }
    CreatBufResource(id);
}

void ResizeSurfBuffer(SurfBufId id) {
    SurfBuffer& item = _surfBufs[id];
    if (item.sizeCode & VARI_SIZE) {
        CreatBufResource(id);
    }
}

void DestorySufBuffer(SurfBufId id) {
    SurfBuffer& item = _surfBufs[id];
    if (item.colBuf) item.colBuf->Destroy(); item.colBuf = nullptr;
    if (item.depBuf) item.depBuf->Destroy(); item.depBuf = nullptr;
}

ColorBuffer* GetColBuf(SurfBufId id) {
    return _surfBufs[id].colBuf;
}

DepthBuffer* GetDepBuf(SurfBufId id) {
    return _surfBufs[id].depBuf;
}

void ShowDebugWindowMenu() {
    using namespace ImGui;
    MenuItem("Viewable Surfaces", NULL, false, false);
    for (auto& item : _surfDebugShow) {
        char temp[32];
        sprintf_s(temp, 32, "%ls", _bufNames[item.first]);
        if(MenuItem(temp, NULL, item.second)) {item.second = !item.second;}
    }
}

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
    auto longAngle = DirectX::XM_PIDIV2;
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
    Core::g_config.enableDebuglayer = true;
    Core::g_config.enableGPUBasedValidationInDebug = false;
    Core::g_config.swapChainDesc.Width = _width;
    Core::g_config.swapChainDesc.Height = _height;
    Core::g_config.swapChainDesc.BufferCount = 5;
    Core::g_config.passThroughMsg = true;
    Core::g_config.useSceneBuf = false;
}

HRESULT
KinectVisualizer::OnCreateResource()
{
    HRESULT hr = S_OK;
    for (uint8_t i = 0; i < SURFBUF_COUNT; ++i) {
        InitialSurfBuffer((SurfBufId)i);
    }
    _sensorTexGen.OnCreateResource(_uploadHeapAlloc);
    _bilateralFilter.OnCreateResoure(_uploadHeapAlloc);
    _tsdfVolume.CreateResource(DEPTH_RESO, _uploadHeapAlloc);
    _normalGen.OnCreateResource(_uploadHeapAlloc);
    _fastICP.OnCreateResource(DEPTH_RESO);
    OnSizeChanged();
    _ResizeVisWin();
    return hr;
}

HRESULT
KinectVisualizer::OnSizeChanged()
{
    return S_OK;
}

void
KinectVisualizer::OnUpdate()
{
    using namespace ImGui;
    _windowActive = false;
    _camera.ProcessInertia();

    ImVec2 f2ScreenSize = ImVec2((float)Graphics::g_SceneColorBuffer.GetWidth(),
        (float)Graphics::g_SceneColorBuffer.GetHeight());
    float fPanelWidth = 350.f;
    float fPanelHeight = f2ScreenSize.y;

    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus;
    SetNextWindowPos(ImVec2(f2ScreenSize.x - fPanelWidth, 0));
    SetNextWindowSize(ImVec2(fPanelWidth, fPanelHeight),
        ImGuiSetCond_Always);
    static bool showPanel = true;
    if (Begin("KinectVisualizer", &showPanel, window_flags)) {
        if (BeginMenu("Debug textures")) {
            ShowDebugWindowMenu();
            ImGui::EndMenu();
        }
        _fastICP.RenderGui();
        Reduction::RenderGui();
        _sensorTexGen.RenderGui();
        SeperableFilter::RenderGui();
        NormalGenerator::RenderGui();
        _tsdfVolume.RenderGui();
        Graphics::UpdateGUI();
    }
    End();
    SetNextWindowPos(ImVec2(0, 0));
    SetNextWindowSize(ImVec2(f2ScreenSize.x - fPanelWidth, f2ScreenSize.y),
        ImGuiSetCond_Always);
    window_flags |= ImGuiWindowFlags_NoScrollbar;
    if (Begin("Visualize Image", nullptr, window_flags)) {
        _visWinPos = GetCursorScreenPos();
        _visWinSize = GetContentRegionAvail();
        _visualize = true;
        if (_visWinSize.x != _width || _visWinSize.y != _height) {
            _width = static_cast<uint16_t>(_visWinSize.x);
            _height = static_cast<uint16_t>(_visWinSize.y);
            _ResizeVisWin();
        }
        ImTextureID tex_id = (void*)GetColBuf(VISUAL_NORMAL);
        ImDrawList* draw_list = GetWindowDrawList();
        InvisibleButton("canvas", _visWinSize);
        ImVec2 bbmin = _visWinPos;
        ImVec2 bbmax =
            ImVec2(_visWinPos.x + _visWinSize.x, _visWinPos.y + _visWinSize.y);
        draw_list->AddImage(tex_id, bbmin, bbmax);
        if (IsItemHovered()) {
            _windowActive = true;
        }
    } else {
        _visualize = false;
    }
    End();

    for (auto& item : _surfDebugShow) {
        if (item.second) {
            GetColBuf((SurfBufId)item.first)->GuiShow(&item.second);
        }
    }
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

    BeginTrans(cptCtx, *GetColBuf(FILTERED_DEPTH), RTV);
    BeginTrans(cptCtx, *GetColBuf(TSDF_NORMAL), UAV);
    BeginTrans(cptCtx, *GetColBuf(KINECT_NORMAL), UAV);
    Trans(cptCtx, *GetColBuf(WEIGHT), UAV);

    // Request depthmap for ICP
    _tsdfVolume.ExtractSurface(gfxCtx, GetColBuf(TSDF_DEPTH),
        _visualize ? GetColBuf(VISUAL_DEPTH)
                    : nullptr, GetDepBuf(VISUAL_DEPTH));
    BeginTrans(cptCtx, *GetColBuf(TSDF_DEPTH), csSRV);
    cptCtx.ClearUAV(*GetColBuf(WEIGHT), ClearVal);
    if (_visualize) {
        // Generate normalmap for visualized depthmap
        _normalGen.OnProcessing(cptCtx, L"Norm_Vis",
            GetColBuf(VISUAL_DEPTH), GetColBuf(VISUAL_NORMAL));
        _tsdfVolume.RenderDebugGrid(
            gfxCtx, GetColBuf(VISUAL_NORMAL), GetDepBuf(VISUAL_DEPTH));
    }
    BeginTrans(cptCtx, *GetColBuf(WEIGHT), UAV);
    // Generate normalmap for TSDF depthmap
    _normalGen.OnProcessing(cptCtx, L"Norm_TSDF",
        &*GetColBuf(TSDF_DEPTH), GetColBuf(TSDF_NORMAL));
    BeginTrans(cptCtx, *GetColBuf(TSDF_DEPTH), csSRV);
    BeginTrans(cptCtx, *GetColBuf(TSDF_NORMAL), csSRV);

    // Pull new data from Kinect
    bool newData = _sensorTexGen.OnRender(cmdCtx, GetColBuf(KINECT_DEPTH),
        GetColBuf(KINECT_COLOR), GetColBuf(KINECT_INFRA),
        GetColBuf(KINECT_DEPTH_VIS));
    cmdCtx.Flush();
    _tsdfVolume.UpdateGPUMatrixBuf(cptCtx, _sensorTexGen.GetVCamMatrixBuf());
    // Bilateral filtering
    _bilateralFilter.OnRender(gfxCtx, L"Filter_Raw",
        GetColBuf(KINECT_DEPTH), GetColBuf(FILTERED_DEPTH), GetColBuf(WEIGHT));

    BeginTrans(cptCtx, *GetColBuf(WEIGHT), csSRV);
    if (_bilateralFilter.IsEnabled()) {
        BeginTrans(cptCtx, *GetColBuf(FILTERED_DEPTH), csSRV);
    }
    // TSDF volume updating
    _tsdfVolume.UpdateVolume(
        cptCtx, GetColBuf(KINECT_DEPTH), GetColBuf(WEIGHT));

    BeginTrans(cptCtx, *GetColBuf(WEIGHT), UAV);
    // Defragment active block queue in TSDF
    _tsdfVolume.DefragmentActiveBlockQueue(cptCtx);

    // Generate normalmap for Kinect depthmap
    _normalGen.OnProcessing(cptCtx, L"Norm_Raw",
        _bilateralFilter.IsEnabled() ? GetColBuf(FILTERED_DEPTH)
                                     : GetColBuf(KINECT_DEPTH),
        GetColBuf(KINECT_NORMAL), GetColBuf(WEIGHT));
    {
        GPU_PROFILE(cptCtx, L"ICP");
        for (uint8_t i = 0; i < 10; ++i) {
            _fastICP.OnProcessing(cptCtx, i, GetColBuf(WEIGHT),
                GetColBuf(TSDF_DEPTH), GetColBuf(TSDF_NORMAL),
                GetColBuf(FILTERED_DEPTH), GetColBuf(KINECT_NORMAL));
            _fastICP.OnSolving();
        }
    }
}

void
KinectVisualizer::OnDestroy()
{
    for (uint8_t i = 0; i < SURFBUF_COUNT; ++i) {
        DestorySufBuffer((SurfBufId)i);
    }
    _sensorTexGen.OnDestory();
    _normalGen.OnDestory();
    _tsdfVolume.Destory();
    _bilateralFilter.OnDestory();
    _fastICP.OnDestory();
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
        _camera.ZoomRadius(-0.001f*delta);
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

    for (uint8_t i = 0; i < SURFBUF_COUNT; ++i) {
        ResizeSurfBuffer((SurfBufId)i);
    }
}