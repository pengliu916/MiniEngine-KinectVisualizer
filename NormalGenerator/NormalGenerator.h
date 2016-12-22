#pragma once
class NormalGenerator
{
#include "NormalGenerator.inl"
public:
    static void RenderGui();

    NormalGenerator(uint2 reso, const std::wstring& name);
    ~NormalGenerator();
    void OnCreateResource();
    void OnDestory();
    void OnResize(uint2 reso);
    void OnProcessing(ComputeContext& cptCtx, ColorBuffer* pInputTex);

    ColorBuffer* GetNormalMap();

private:
    std::wstring _name;
    CBuffer _dataCB;
    ColorBuffer _normalMap;
};