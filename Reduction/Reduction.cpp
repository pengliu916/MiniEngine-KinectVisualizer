#include "pch.h"
#include "Reduction.h"

#define Trans(ctx, res, state) \
ctx.TransitionResource(res, state)

#define Bind(ctx, rootIdx, offset, count, resource) \
ctx.SetDynamicDescriptors(rootIdx, offset, count, resource)

#define BeginTrans(ctx, res, state) \
ctx.BeginResourceTransition(res, state)

namespace {
enum THREAD : int {
    kT32 = 0,
    kT64,
    kT128,
    kT256,
    kT512,
    kT1024,
    kTNum
};
enum FETCH : int{
    kF1 = 0,
    kF2,
    kF4,
    kF8,
    kF16,
    kFNum
};
enum PASSES : int {
    kAtomicAdd = 0,
    kDone1Goup,
    kAdaptive,
    kPNum
};

typedef D3D12_RESOURCE_STATES State;
const State UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
const State SRV = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
const State SRC = D3D12_RESOURCE_STATE_COPY_SOURCE;

RootSignature _rootsig;
ComputePSO _cptPSO[Reduction::kTypes][kTNum][kFNum][kPNum];
std::once_flag _psoCompiled_flag;

THREAD _threadPerTG = kT128;
FETCH _fetchPerThread = kF4;
PASSES _reductionPassMode = kDone1Goup;

inline uint32_t _GetReductionRate()
{
    return 32 * (1 << _threadPerTG) * (1 << _fetchPerThread);
}

inline HRESULT _Compile(LPCWSTR shaderName,
    const D3D_SHADER_MACRO* macro, ID3DBlob** bolb)
{
    UINT compilerFlag = D3DCOMPILE_OPTIMIZATION_LEVEL3;
#if defined(DEBUG) || defined(_DEBUG)
    compilerFlag = D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_DEBUG;
#endif
    int iLen = (int)wcslen(shaderName);
    char target[8];
    wchar_t fileName[128];
    swprintf_s(fileName, 128, L"Reduction_%ls.hlsl", shaderName);
    switch (shaderName[iLen - 2]) {
    case 'c': sprintf_s(target, 8, "cs_5_1"); break;
    case 'p': sprintf_s(target, 8, "ps_5_1"); break;
    case 'v': sprintf_s(target, 8, "vs_5_1"); break;
    default:
        PRINTERROR(L"Shader name: %s is Invalid!", shaderName);
    }
    return Graphics::CompileShaderFromFile(Core::GetAssetFullPath(
        fileName).c_str(), macro, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main",
        target, compilerFlag, 0, bolb);
}


void _CreatePSO(Reduction::TYPE type, THREAD t, FETCH f, PASSES p)
{
    using namespace Microsoft::WRL;
    HRESULT hr;
    ComPtr<ID3DBlob> reductionCS;
    D3D_SHADER_MACRO macros[] = {
        {"__hlsl", "1"},
        {"TYPE", ""},
        {"THREAD", ""},
        {"FETCH_COUNT", ""},
        {"ATOMIC_ACCU", ""},
        {"DATAWIDTH", ""},
        {nullptr, nullptr}
    };
    macros[4].Definition = p == kAtomicAdd ? "1" : "0";
    switch (type)
    {
    case Reduction::kFLOAT4:
        macros[1].Definition = "float4";
        macros[5].Definition = "4"; break;
    case Reduction::kFLOAT:
        macros[1].Definition = "float";
        macros[5].Definition = "1"; break;
    default: PRINTERROR("Reduction Type is invalid"); break;
    }
    char cthread[32];
    sprintf_s(cthread, 32, "%d", 32 * (1 << t));
    macros[2].Definition = cthread;
    char cfetch[32];
    sprintf_s(cfetch, 32, "%d", 1 << f);
    macros[3].Definition = cfetch;
    V(_Compile(L"Reduction_cs", macros, &reductionCS));
    _cptPSO[type][t][f][p].SetRootSignature(_rootsig);
    _cptPSO[type][t][f][p].SetComputeShader(
        reductionCS->GetBufferPointer(),reductionCS->GetBufferSize());
    _cptPSO[type][t][f][p].Finalize();
    PRINTWARN("CreatePSO Called");
}

void _UpdatePSOs()
{
    for (UINT i = 0; i < Reduction::kTypes; ++i)
        for (UINT j = 0; j < kTNum; ++j)
            for (UINT k = 0; k < kFNum; ++k)
                for (UINT w = 0; w < kPNum; ++w)
                    if (_cptPSO[i][j][k][w].GetPipelineStateObject())
                        _CreatePSO((Reduction::TYPE)i,
                            (THREAD)j, (FETCH)k, (PASSES)w);
}

void _CreateStaticResoure()
{
    _rootsig.Reset(3);
    _rootsig[0].InitAsConstants(0, 3);
    _rootsig[1].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0, 1);
    _rootsig[2].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 1);
    _rootsig.Finalize(L"Reduction",
        D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS);
}
}

Reduction::Reduction(TYPE type, uint32_t size, uint32_t bufCount)
    : _type(type), _size(size), _bufCount(bufCount)
{
    switch (_type)
    {
    case kFLOAT4: _elementSize = 4 * sizeof(float); break;
    case kFLOAT: _elementSize = sizeof(float); break;
    default: PRINTERROR("Reduction Type is invalid"); break;
    }
}

Reduction::~Reduction()
{
}

void
Reduction::OnCreateResource()
{
    std::call_once(_psoCompiled_flag, _CreateStaticResoure);
    _reduceRatio = _GetReductionRate();
    _CreateIntermmediateBuf();
    _resultBuf.Create(L"Reduction_Result", _bufCount, _elementSize);
    _resultAtomicBuf.Create(L"Reduction_ResultAtomic", _bufCount, _elementSize);
    _readBackBuf.Create(L"Reduction_Readback", _bufCount, _elementSize);
}

void
Reduction::OnDestory()
{
    _tempResultBuf[0].Destroy();
    _tempResultBuf[1].Destroy();
    _resultBuf.Destroy();
    _resultAtomicBuf.Destroy();
    _readBackBuf.Destroy();
}

void
Reduction::ProcessingOneBuffer(ComputeContext& cptCtx,
    StructuredBuffer* pInputBuf, uint32_t bufId)
{
    uint32_t newReductionRate = _GetReductionRate();
    if (_reduceRatio != newReductionRate) {
        _reduceRatio = newReductionRate;
        _CreateIntermmediateBuf();
    }
    Trans(cptCtx, *pInputBuf, SRV);
    uint8_t curBuf = 0;
    cptCtx.SetRootSignature(_rootsig);
    PASSES p = _reductionPassMode == kAtomicAdd ? kAtomicAdd : kDone1Goup;
    Bind(cptCtx, 2, 0, 1, &pInputBuf->GetSRV());
    UINT bufSize = _size;
    UINT groupCount = (_size + _reduceRatio - 1) / _reduceRatio;
    if (_reductionPassMode != kAtomicAdd) {
        UINT threshold = _reductionPassMode == kDone1Goup ? 1 : _reduceRatio;
        ComputePSO& PSO = _cptPSO[_type][_threadPerTG][_fetchPerThread][p];
        if (!PSO.GetPipelineStateObject()) {
            _CreatePSO(_type, _threadPerTG, _fetchPerThread, p);
        }
        cptCtx.SetPipelineState(PSO);
        while (groupCount > threshold) {
            Trans(cptCtx, _tempResultBuf[curBuf], UAV);
            Bind(cptCtx, 1, 0, 1, &_tempResultBuf[curBuf].GetUAV());
            cptCtx.SetConstants(
                0, DWParam(bufSize), DWParam(bufId), DWParam(0));
            cptCtx.Dispatch(groupCount);
            Trans(cptCtx, _tempResultBuf[curBuf], SRV);
            Bind(cptCtx, 2, 0, 1, &_tempResultBuf[curBuf].GetSRV());
            curBuf = 1 - curBuf;
            bufSize = groupCount;
            groupCount = (groupCount + _reduceRatio -  1) / _reduceRatio;
        }
    }
    if (_reductionPassMode == kDone1Goup) {
        Trans(cptCtx, _resultBuf, UAV);
        Bind(cptCtx, 1, 0, 1, &_resultBuf.GetUAV());
        cptCtx.SetConstants(
            0, DWParam(bufSize), DWParam(bufId), DWParam(groupCount));
    } else {
        ComputePSO& PSO =
            _cptPSO[_type][_threadPerTG][_fetchPerThread][kAtomicAdd];
        if (!PSO.GetPipelineStateObject()) {
            _CreatePSO(_type, _threadPerTG, _fetchPerThread, kAtomicAdd);
        }
        cptCtx.SetPipelineState(PSO);
        Trans(cptCtx, _resultAtomicBuf, UAV);
        Bind(cptCtx, 1, 0, 1, &_resultAtomicBuf.GetUAV());
        cptCtx.SetConstants(0, DWParam(bufSize),
            DWParam(bufId * _elementSize), DWParam(groupCount));
    }
    cptCtx.Dispatch(groupCount);
}

void
Reduction::ClearResultBuf(ComputeContext& cptCtx)
{
    static UINT ClearValue[4] = {};
    if (_reductionPassMode != kDone1Goup) {
        cptCtx.ClearUAV(_resultAtomicBuf, ClearValue);
        BeginTrans(cptCtx, _resultAtomicBuf, UAV);
    }
}

void
Reduction::PrepareResult(ComputeContext& cptCtx)
{
    if (_reductionPassMode == kDone1Goup) {
        Trans(cptCtx, _resultBuf, SRC);
        cptCtx.CopyBufferRegion(
            _readBackBuf, 0, _resultBuf, 0, _bufCount * _elementSize);
        BeginTrans(cptCtx, _resultBuf, UAV);
    } else {
        Trans(cptCtx, _resultAtomicBuf, SRC);
        cptCtx.CopyBufferRegion(
            _readBackBuf, 0, _resultAtomicBuf, 0, _bufCount * _elementSize);
        BeginTrans(cptCtx, _resultAtomicBuf, UAV);
    }
    _readBackFence = cptCtx.Flush();
}

void
Reduction::ReadLastResult(float* result)
{
    Graphics::g_cmdListMngr.WaitForFence(_readBackFence);
    D3D12_RANGE range = {0, _elementSize};
    D3D12_RANGE umapRange = {};
    _readBackBuf.Map(&range, &_readBackPtr);
    memcpy(result, _readBackPtr, _bufCount * _elementSize);
    _readBackBuf.Unmap(&umapRange);
    return;
}

void
Reduction::RenderGui()
{
    using namespace ImGui;
    if (!CollapsingHeader("Reduction")) {
        return;
    }
    if (Button("ReconpileShaders##Reduction")) {
        _UpdatePSOs();
    }
    Separator();
    Columns(2);
    Text("GThread Num"); NextColumn();
    Text("Fetch Num"); NextColumn();
    RadioButton("32##Reduction", (int*)&_threadPerTG, kT32); NextColumn();
    RadioButton("1##Reduction", (int*)&_fetchPerThread, kF1); NextColumn();
    RadioButton("64##Reduction", (int*)&_threadPerTG, kT64); NextColumn();
    RadioButton("2##Reduction", (int*)&_fetchPerThread, kF2); NextColumn();
    RadioButton("128##Reduction", (int*)&_threadPerTG, kT128); NextColumn();
    RadioButton("4##Reduction", (int*)&_fetchPerThread, kF4); NextColumn();
    RadioButton("256##Reduction", (int*)&_threadPerTG, kT256); NextColumn();
    RadioButton("8##Reduction", (int*)&_fetchPerThread, kF8); NextColumn();
    RadioButton("512##Reduction", (int*)&_threadPerTG, kT512); NextColumn();
    RadioButton("16##Reduction", (int*)&_fetchPerThread, kF16); NextColumn();
    RadioButton("1024##Reduction", (int*)&_threadPerTG, kT1024); NextColumn();
    Columns(1);
    Separator();
    RadioButton("AtomicAdd##Reduction", (int*)&_reductionPassMode, kAtomicAdd);
    RadioButton("Recursive##Reduction", (int*)&_reductionPassMode, kDone1Goup);
    RadioButton("Adaptive##Reduction", (int*)&_reductionPassMode, kAdaptive);
}

void
Reduction::_CreateIntermmediateBuf()
{
    _tempResultBuf[0].Destroy();
    _tempResultBuf[1].Destroy();
    _tempResultBuf[0].Create(L"Reduction",
        (_size + _reduceRatio - 1) / _reduceRatio, _elementSize);
    _tempResultBuf[1].Create(L"Reduction",
        (_size + _reduceRatio - 1) / _reduceRatio, _elementSize);
}