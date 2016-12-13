#pragma once
class NormalGenerator
{
#include "NormalGenerator.inl"
public:
    NormalGenerator(uint2 reso, const std::wstring& name);
    ~NormalGenerator();
    void OnCreateResource();
    void OnDestory();
    void OnResize(uint2 reso);
    void OnProcessing(ComputeContext& cptCtx, ColorBuffer* pInputTex);
    void RenderGui();

    ColorBuffer* GetNormalMap();

private:
    std::wstring _name;
    CBuffer _dataCB;
    ColorBuffer _normalMap;
};