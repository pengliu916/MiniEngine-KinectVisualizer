#pragma once
class Reduction
{
public:
    enum TYPE{
        kFLOAT4 = 0,
        kFLOAT = 1,
        kTypes
    };
    
    static void RenderGui();
    Reduction(TYPE type, uint32_t size, uint32_t bufCount = 1);
    ~Reduction();
    void OnCreateResource();
    void OnDestory();
    void ProcessingOneBuffer(ComputeContext& cptCtx,
        StructuredBuffer* pInputBuf, uint32_t bufId = 0);
    void PrepareResult(ComputeContext& cptCtx);
    void ReadLastResult(float* result);

private:
    void _CreateIntermmediateBuf(uint32_t elementSize);
    const TYPE _type;
    const uint32_t _size;
    const uint32_t _bufCount;
    uint32_t _currentReductionRate;
    StructuredBuffer _intermmediateResultBuf[2];
    StructuredBuffer _finalResultBuf;
    ReadBackBuffer _readBackBuf;
    void* _readBackPtr;
    uint64_t _readBackFence = 0;
};