#pragma once
class Reduction;
class FastICP
{
#include "FastICP.inl"
public:

    FastICP();
    ~FastICP();
    void OnCreateResource(uint2 inputReso);
    void OnDestory();
    void OnProcessing(ComputeContext& cptCtx, uint8_t iteration,
        ColorBuffer* pConfidence,
        ColorBuffer* pTSDFDepth, ColorBuffer* pTSDFNormal,
        ColorBuffer* pKinectDepth, ColorBuffer* pKinectNormal);
    void OnSolving();
    void RenderGui();

private:
    void _CreatePrepareBuffers(uint2 u2Reso);

    Reduction* _pReductionExec = nullptr;
    StructuredBuffer _dataPackBuf[DATABUF_COUNT];
    float _reductionResult[DATABUF_COUNT * 4];
    CBuffer _dataCB;
};