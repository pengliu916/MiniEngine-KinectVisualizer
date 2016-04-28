#pragma once
#include "DX12Framework.h"
class KinectVisualizer :public Core::IDX12Framework
{
public:
	KinectVisualizer( uint32_t width, uint32_t height, std::wstring name );
	~KinectVisualizer();

	virtual void OnConfiguration();
	virtual HRESULT OnCreateResource();
	virtual HRESULT OnSizeChanged();
	virtual void OnUpdate();
	virtual void OnRender( CommandContext& EngineContext );
	virtual void OnDestroy();
	virtual bool OnEvent( MSG* msg );

protected:
	uint16_t m_width;
	uint16_t m_height;
};

