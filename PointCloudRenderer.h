#pragma once
class PointCloudRenderer
{
#include "PointCloudRenderer_SharedHeader.inl"
public:
	enum RenderMode
	{
		kPointCloud = 0,
		kSurface = 1,
		kShadedSurface = 2,
		kNumRenderMode
	};
	
	PointCloudRenderer();
	~PointCloudRenderer();
	HRESULT OnCreateResource();
	void UpdateCB( DirectX::XMFLOAT2 ColReso, DirectX::XMFLOAT2 DepReso,
		DirectX::XMFLOAT4 ColFC, DirectX::XMFLOAT4 DepFC, DirectX::XMMATRIX Dep2Col );
	void OnRender( GraphicsContext& gfxContext, ColorBuffer* pDepthMap, ColorBuffer* pColorMap,
		DirectX::XMMATRIX viewProj);

	RenderMode m_RenderMode = kPointCloud;
	CBuffer m_CBData;

protected:
	GraphicsPSO m_RenderModePSO[kNumRenderMode];
	RootSignature m_RootSignature;
};

