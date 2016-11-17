#include "pch.h"
#include "KinectVisualizer.h"
#include "CalibData.inl"
#include <ppl.h>

using namespace DirectX;
using namespace Microsoft::WRL;

namespace {
    bool _BilateralFilter = false;
}

KinectVisualizer::KinectVisualizer(
    uint32_t width, uint32_t height, std::wstring name)
    :_sensorTexGen(true, true, true)
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
    OnSizeChanged();

    return S_OK;
}

HRESULT KinectVisualizer::OnSizeChanged()
{
    uint32_t width = Core::g_config.swapChainDesc.Width;
    uint32_t height = Core::g_config.swapChainDesc.Height;

    float fAspectRatio = width / (FLOAT)height;
    _camera.Projection(XM_PIDIV2 / 2, fAspectRatio);
    return S_OK;
}

void KinectVisualizer::OnUpdate()
{
    _camera.ProcessInertia();
    static bool showPanel = true;
    if (ImGui::Begin("KinectVisualizer", &showPanel)) {
        ImGui::Checkbox("Bilateral Filter", &_BilateralFilter);
        if (_BilateralFilter) {
            _bilateralFilter.RenderGui();
        }
        _pointCloudRenderer.RenderGui();
        _sensorTexGen.RenderGui();
    }
    ImGui::End();
}

void KinectVisualizer::OnRender(CommandContext & EngineContext)
{
    EngineContext.BeginResourceTransition(Graphics::g_SceneColorBuffer,
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    EngineContext.BeginResourceTransition(Graphics::g_SceneDepthBuffer,
        D3D12_RESOURCE_STATE_DEPTH_WRITE);
    if (_sensorTexGen.OnRender(EngineContext) && _BilateralFilter) {
        _bilateralFilter.OnRender(EngineContext.GetGraphicsContext(), 
            _sensorTexGen.GetOutTex(SensorTexGen::kDepthTex));
    }
    // Camera return row major matrix, so will be transposed version in this
    // application since we use column major
    XMMATRIX view_T = _camera.View();
    XMMATRIX proj_T = _camera.Projection();
    XMFLOAT4 viewPos;
    XMStoreFloat4(&viewPos, _camera.Eye());
    _pointCloudRenderer.UpdateLightPos(viewPos);
    _pointCloudRenderer.OnRender(EngineContext.GetGraphicsContext(),
        _sensorTexGen.GetOutTex(SensorTexGen::kDepthTex),
        _sensorTexGen.GetOutTex(SensorTexGen::kColorTex),
        XMMatrixMultiply(view_T,proj_T));
}

void KinectVisualizer::OnDestroy()
{
    _pointCloudRenderer.OnDestory();
    _bilateralFilter.OnDestory();
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