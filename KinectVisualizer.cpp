#include "pch.h"
#include "KinectVisualizer.h"
#include "CalibData.inl"
#include <ppl.h>

using namespace DirectX;
using namespace Microsoft::WRL;

namespace
{
	bool _BilateralFilter = false;
}

KinectVisualizer::KinectVisualizer( uint32_t width, uint32_t height, std::wstring name )
	:m_SensorTexGen(true,true,true)
{
	m_width = width;
	m_height = height;

	auto center = XMVectorSet( 0.0f, 0.0f, 0.0f, 0.0f );
	auto radius = m_camOrbitRadius;
	auto minRadius = m_camMinOribtRadius;
	auto maxRadius = m_camMaxOribtRadius;
	auto longAngle = -DirectX::XM_PIDIV2;
	auto latAngle = DirectX::XM_PIDIV2;
	m_Camera.View( center, radius, minRadius, maxRadius, longAngle, latAngle );
}

KinectVisualizer::~KinectVisualizer()
{
}

void KinectVisualizer::OnConfiguration()
{
	Core::g_config.FXAA = false;
	Core::g_config.swapChainDesc.Width = m_width;
	Core::g_config.swapChainDesc.Height = m_height;
	Core::g_config.swapChainDesc.BufferCount = 5;
}

HRESULT KinectVisualizer::OnCreateResource()
{
	// Create resource for pointcloudrenderer
	m_SensorTexGen.OnCreateResource();
	uint16_t ColWidth, ColHeight, DepWidth, DepHeight;
	m_SensorTexGen.GetColorReso( ColWidth, ColHeight );
	m_SensorTexGen.GetDepthInfrareReso( DepWidth, DepHeight );

	m_PointCloudRenderer.OnCreateResource();
	m_PointCloudRenderer.UpdateCB( XMFLOAT2( ColWidth, ColHeight ), XMFLOAT2( DepWidth, DepHeight ),
		XMFLOAT4( COLOR_C.x, COLOR_C.y, COLOR_F.x, COLOR_F.y ),
		XMFLOAT4( DEPTH_C.x, DEPTH_C.y, DEPTH_F.x, DEPTH_F.y ),
		DEPTH2COLOR_MAT );

	// Create resource for BilateralFilter
	m_BilateralFilter.OnCreateResoure( DXGI_FORMAT_R16_UINT );
	m_BilateralFilter.UpdateCB( XMUINT2( DepWidth, DepHeight ), DXGI_FORMAT_R16_UINT );
	OnSizeChanged();

	return S_OK;
}

HRESULT KinectVisualizer::OnSizeChanged()
{
	uint32_t width = Core::g_config.swapChainDesc.Width;
	uint32_t height = Core::g_config.swapChainDesc.Height;

	float fAspectRatio = width / (FLOAT)height;
	m_Camera.Projection( XM_PIDIV2 / 2, fAspectRatio );
	return S_OK;
}

void KinectVisualizer::OnUpdate()
{
	m_Camera.ProcessInertia();

	static bool showPanel = true;
	
	if (ImGui::Begin( "KinectVisualizer", &showPanel ))
	{
		ImGui::Checkbox( "Bilateral Filter", &_BilateralFilter );
		if (_BilateralFilter) m_BilateralFilter.RenderGui();
		m_PointCloudRenderer.RenderGui();
		m_SensorTexGen.RenderGui();
	}
	ImGui::End();
}

void KinectVisualizer::OnRender( CommandContext & EngineContext )
{
	m_SensorTexGen.OnRender( EngineContext );
	if (_BilateralFilter)
		m_BilateralFilter.OnRender( EngineContext.GetGraphicsContext(), &m_SensorTexGen.m_OutTexture[SensorTexGen::kDepthTex] );

	XMMATRIX view = m_Camera.View();
	XMMATRIX proj = m_Camera.Projection();
	XMFLOAT4 viewPos;
	XMStoreFloat4( &viewPos, m_Camera.Eye() );
	m_PointCloudRenderer.m_CBData.LightPos = viewPos;
	m_PointCloudRenderer.OnRender( EngineContext.GetGraphicsContext(), &m_SensorTexGen.m_OutTexture[SensorTexGen::kDepthTex], 
		&m_SensorTexGen.m_OutTexture[SensorTexGen::kColorTex],
		XMMatrixMultiply( view, proj ) );
}

void KinectVisualizer::OnDestroy()
{
	m_SensorTexGen.OnDestory();
}

bool KinectVisualizer::OnEvent( MSG * msg )
{
	switch (msg->message)
	{
	case WM_MOUSEWHEEL:
	{
		auto delta = GET_WHEEL_DELTA_WPARAM( msg->wParam );
		m_Camera.ZoomRadius( -0.01f*delta );
	}
	case WM_POINTERDOWN:
	case WM_POINTERUPDATE:
	case WM_POINTERUP:
	{
		auto pointerId = GET_POINTERID_WPARAM( msg->wParam );
		POINTER_INFO pointerInfo;
		if (GetPointerInfo( pointerId, &pointerInfo )) {
			if (msg->message == WM_POINTERDOWN) {
				// Compute pointer position in render units
				POINT p = pointerInfo.ptPixelLocation;
				ScreenToClient( Core::g_hwnd, &p );
				RECT clientRect;
				GetClientRect( Core::g_hwnd, &clientRect );
				p.x = p.x * Core::g_config.swapChainDesc.Width / (clientRect.right - clientRect.left);
				p.y = p.y * Core::g_config.swapChainDesc.Height / (clientRect.bottom - clientRect.top);
				// Camera manipulation
				m_Camera.AddPointer( pointerId );
			}
		}

		// Otherwise send it to the camera controls
		m_Camera.ProcessPointerFrames( pointerId, &pointerInfo );
		if (msg->message == WM_POINTERUP) m_Camera.RemovePointer( pointerId );
	}
	}
	return false;
}
