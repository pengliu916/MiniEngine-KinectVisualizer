#pragma once
#include "DX12Framework.h"
#include "SensorTexGen\SensorTexGen.h"
#include "PointCloudRenderer\PointCloudRenderer.h"
#include "SeparableFilter\SeparableFilter.h"

class KinectVisualizer :public Core::IDX12Framework
{
public:
    KinectVisualizer(uint32_t width, uint32_t height, std::wstring name);
    ~KinectVisualizer();

    virtual void OnConfiguration();
    virtual HRESULT OnCreateResource();
    virtual HRESULT OnSizeChanged();
    virtual void OnUpdate();
    virtual void OnRender(CommandContext& EngineContext);
    virtual void OnDestroy();
    virtual bool OnEvent(MSG* msg);

private:
    PointCloudRenderer _pointCloudRenderer;
    SeperableFilter _bilateralFilter;
    SensorTexGen _sensorTexGen;

    uint16_t _width;
    uint16_t _height;

    // For camera
    OrbitCamera _camera;
    float _camOrbitRadius = 7.f;
    float _camMaxOribtRadius = 50.f;
    float _camMinOribtRadius = 0.1f;
};