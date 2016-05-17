#pragma once
#include "DX12Framework.h"
#include "SensorTexGen.h"
#include "PointCloudRenderer.h"
#include "SeparableFilter.h"

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

	PointCloudRenderer		m_PointCloudRenderer;
	SeperableFilter			m_BilateralFilter;
	SensorTexGen			m_SensorTexGen;

protected:
	uint16_t m_width;
	uint16_t m_height;
	
	// For camera
	OrbitCamera				m_Camera;
	float					m_camOrbitRadius = 10.f;
	float					m_camMaxOribtRadius = 50.f;
	float					m_camMinOribtRadius = 0.1f;
};