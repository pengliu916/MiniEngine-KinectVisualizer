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
    void ClearResultBuf(ComputeContext& cptCtx);
    void ReadLastResult(float* result);

private:
    void _CreateIntermmediateBuf();
    const TYPE _type;
    const uint32_t _size;
    const uint32_t _bufCount;
    uint32_t _elementSize;
    uint32_t _reduceRatio;
    StructuredBuffer _tempResultBuf[2];
    StructuredBuffer _resultBuf;
    ByteAddressBuffer _resultAtomicBuf;
    ReadBackBuffer _readBackBuf;
    void* _readBackPtr;
    uint64_t _readBackFence = 0;
};