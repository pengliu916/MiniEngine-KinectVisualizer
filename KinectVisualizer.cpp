#include "pch.h"
#include "KinectVisualizer.h"


KinectVisualizer::KinectVisualizer( uint32_t width, uint32_t height, std::wstring name )
{
	m_width = width;
	m_height = height;
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
	return E_NOTIMPL;
}

HRESULT KinectVisualizer::OnSizeChanged()
{
	return E_NOTIMPL;
}

void KinectVisualizer::OnUpdate()
{
}

void KinectVisualizer::OnRender( CommandContext & EngineContext )
{
}

void KinectVisualizer::OnDestroy()
{
}

bool KinectVisualizer::OnEvent( MSG * msg )
{
	return false;
}
