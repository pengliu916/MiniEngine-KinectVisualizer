#include "pch.h"
#include "KinectVisualizer.h"
#include "CalibData.inl"
#include <ppl.h>

using namespace DirectX;
using namespace Microsoft::WRL;

namespace {
    enum OnShow {
        kTSDFVolume = 0,
        kPointCloud,
        kNumItem
    };
    OnShow _itemOnShow = kTSDFVolume;
    DirectX::XMMATRIX _depthViewInv_T =
        DirectX::XMMatrixTranslation(0, 0, 3);
    bool _useBilateralFilter = false;
}

KinectVisualizer::KinectVisualizer(
    uint32_t width, uint32_t height, std::wstring name)
    : _sensorTexGen(true, true, true),
    _normalGen(uint2(512, 424), L"NormalMap")
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

void KinectVisualizer::OnConfiguration()
{
    Core::g_config.FXAA = false;
    Core::g_config.swapChainDesc.Width = _width;
    Core::g_config.swapChainDesc.Height = _height;
    Core::g_config.swapChainDesc.BufferCount = 5;
}

HRESULT KinectVisualizer::OnCreateResource()
{
    // Create resource for pointcloudrenderer
    _sensorTexGen.OnCreateResource();
    uint16_t colWidth, colHeight, depWidth, depHeight;
    _sensorTexGen.GetColorReso(colWidth, colHeight);
    _sensorTexGen.GetDepthInfrareReso(depWidth, depHeight);

    _pointCloudRenderer.OnCreateResource();
    _pointCloudRenderer.UpdateCB(
        XMFLOAT2(colWidth, colHeight), XMFLOAT2(depWidth, depHeight),
        XMFLOAT4(COLOR_C.x, COLOR_C.y, COLOR_F.x, COLOR_F.y),
        XMFLOAT4(DEPTH_C.x, DEPTH_C.y, DEPTH_F.x, DEPTH_F.y),
        DEPTH2COLOR_MAT);

    // Create resource for BilateralFilter
    _bilateralFilter.OnCreateResoure(DXGI_FORMAT_R16_UINT);
    _bilateralFilter.UpdateCB(XMUINT2(depWidth, depHeight));

    int2 depthReso = int2(depWidth, depHeight);
    int2 colorReso = int2(colWidth, colHeight);

    _tsdfVolume.OnCreateResource(depthReso, colorReso);

    _normalGen.OnCreateResource();

    OnSizeChanged();

    return S_OK;
}

HRESULT KinectVisualizer::OnSizeChanged()
{
    uint32_t width = Core::g_config.swapChainDesc.Width;
    uint32_t height = Core::g_config.swapChainDesc.Height;

    float fAspectRatio = width / (FLOAT)height;
    _camera.Projection(XM_PIDIV2 / 2, fAspectRatio);

    _tsdfVolume.OnResize();

    return S_OK;
}

void KinectVisualizer::OnUpdate()
{
    _camera.ProcessInertia();
    static bool showPanel = true;
    if (ImGui::Begin("KinectVisualizer", &showPanel)) {
        ImGui::Checkbox("Bilateral Filter", &_useBilateralFilter);
        if (_useBilateralFilter) {
            _bilateralFilter.RenderGui();
        }
        ImGui::RadioButton("TSDFVolume", (int*)&_itemOnShow, kTSDFVolume);
        ImGui::RadioButton("PointCloud", (int*)&_itemOnShow, kPointCloud);
        switch (_itemOnShow)
        {
        case kPointCloud:
            if (ImGui::CollapsingHeader("Point Cloud")) {
                _pointCloudRenderer.RenderGui();
            }
            break;
        case kTSDFVolume:
            _tsdfVolume.OnUpdate();
            _tsdfVolume.RenderGui();
            break;
        }
        _sensorTexGen.RenderGui();
        _normalGen.RenderGui();
    }
    ImGui::End();
}

void KinectVisualizer::OnRender(CommandContext & EngineContext)
{
    XMMATRIX mView_T = _camera.View();
    XMMATRIX mViewInv_T = XMMatrixInverse(nullptr, mView_T);
    XMMATRIX mProj_T = _camera.Projection();
    XMMATRIX mProjView_T = XMMatrixMultiply(mView_T, mProj_T);
    XMFLOAT4 viewPos;
    XMStoreFloat4(&viewPos, _camera.Eye());

    EngineContext.BeginResourceTransition(Graphics::g_SceneColorBuffer,
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    EngineContext.BeginResourceTransition(Graphics::g_SceneDepthBuffer,
        D3D12_RESOURCE_STATE_DEPTH_WRITE);
    bool newData = _sensorTexGen.OnRender(EngineContext);
    if (newData && _useBilateralFilter) {
        _bilateralFilter.OnRender(EngineContext.GetGraphicsContext(),
            _sensorTexGen.GetOutTex(SensorTexGen::kDepthTex));
    }
    switch (_itemOnShow)
    {
    case kTSDFVolume:
        _tsdfVolume.OnIntegrate(EngineContext,
            _sensorTexGen.GetOutTex(SensorTexGen::kDepthTex),
            _sensorTexGen.GetOutTex(SensorTexGen::kColorTex),
            _depthViewInv_T);
        _tsdfVolume.OnRender(EngineContext, mProj_T, mView_T);
        _tsdfVolume.OnExtractSurface(EngineContext,
            XMMatrixInverse(nullptr, _depthViewInv_T));
        break;
    case kPointCloud:
        _pointCloudRenderer.UpdateLightPos(viewPos);
        _pointCloudRenderer.OnRender(EngineContext.GetGraphicsContext(),
            _sensorTexGen.GetOutTex(SensorTexGen::kDepthTex),
            _sensorTexGen.GetOutTex(SensorTexGen::kColorTex),
            mProjView_T, _depthViewInv_T);
        break;
    default:
        break;
    }
    _normalGen.OnProcessing(EngineContext.GetComputeContext(),
        _sensorTexGen.GetOutTex(SensorTexGen::kDepthTex));
    _normalGen.GetNormalMap()->GuiShow();
}

void KinectVisualizer::OnDestroy()
{
    _sensorTexGen.OnDestory();
}

bool KinectVisualizer::OnEvent(MSG * msg)
{
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
                // Compute pointer position in render units
                POINT p = pointerInfo.ptPixelLocation;
                ScreenToClient(Core::g_hwnd, &p);
                RECT clientRect;
                GetClientRect(Core::g_hwnd, &clientRect);
                p.x = p.x * Core::g_config.swapChainDesc.Width /
                    (clientRect.right - clientRect.left);
                p.y = p.y * Core::g_config.swapChainDesc.Height /
                    (clientRect.bottom - clientRect.top);
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