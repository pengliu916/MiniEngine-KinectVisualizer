#pragma once
#include "IRGBDStreamer.h"
#include "DX12Framework.h"
#include "LinearFrameAllocator.h"
#include "KinectVisualizer_SharedHeader.inl"

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

protected:
	uint16_t m_width;
	uint16_t m_height;
	
	// For camera
	OrbitCamera				m_Camera;
	float					m_camOrbitRadius = 10.f;
	float					m_camMaxOribtRadius = 50.f;
	float					m_camMinOribtRadius = 0.1f;
	
	IRGBDStreamer*			m_pKinect2;

	RenderCB				m_KinectCB;

	ColorBuffer				m_Texture[IRGBDStreamer::kNumBufferTypes + 1]; // 0 ColorVisualize, 1 DepthVisualize, 2 InfraredVisualize, 3 Depth
	LinearFrameAllocator*	m_pFrameAlloc[IRGBDStreamer::kNumBufferTypes];
	KinectBuffer*			m_pKinectBuffer[IRGBDStreamer::kNumBufferTypes];

	D3D12_VIEWPORT			m_DepthInfraredViewport = {};
	D3D12_RECT				m_DepthInfraredScissorRect = {};

	D3D12_VIEWPORT			m_ColorViewport = {};
	D3D12_RECT				m_ColorScissorRect = {};

	GraphicsPSO				m_DepthInfraredPSO;
	GraphicsPSO				m_DepthInfraredUndistortedPSO;
	GraphicsPSO				m_ColorPSO;
	GraphicsPSO				m_ColorUndistortedPSO;
	ComputePSO				m_DepthInfraredCSPSO;
	ComputePSO				m_DepthInfraredUndistortedCSPSO;
	ComputePSO				m_ColorCSPSO;
	ComputePSO				m_ColorUndistortedCSPSO;
	RootSignature			m_RootSignature;


};