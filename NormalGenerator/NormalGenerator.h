#pragma once
class NormalGenerator
{
#include "NormalGenerator.inl"
public:
    static void RenderGui();

    NormalGenerator();
    ~NormalGenerator();
    void OnCreateResource(LinearAllocator& uploadHeapAlloc);
    void OnDestory();
    void OnProcessing(ComputeContext& cptCtx, const std::wstring& procName,
        ColorBuffer* pInputTex, ColorBuffer* pOutputTex,
        ColorBuffer* pConfidenceTex = nullptr);

private:
    DynAlloc* _pUploadCB;
    ByteAddressBuffer _gpuCB;
    CBuffer _dataCB;
};