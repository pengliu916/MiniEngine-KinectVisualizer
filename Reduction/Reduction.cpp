#include "pch.h"
#include "Reduction.h"

#define Trans(ctx, res, state) \
ctx.TransitionResource(res, state)

#define Bind(ctx, rootIdx, offset, count, resource) \
ctx.SetDynamicDescriptors(rootIdx, offset, count, resource)

namespace {
typedef D3D12_RESOURCE_STATES State;
const State UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
const State SRV = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

RootSignature _rootsig;
ComputePSO _cptReductionPSO[Reduction::kTypes];
std::once_flag _psoCompiled_flag;

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

void _CreatePSO()
{
    using namespace Microsoft::WRL;
    HRESULT hr;
    ComPtr<ID3DBlob> reductionCS[Reduction::kTypes];
    D3D_SHADER_MACRO macros[] = {
        {"__hlsl", "1"},
        {"TYPE", ""},
        {nullptr, nullptr}
    };
    wchar_t temp[32];
    for (UINT i = 0; i < Reduction::kTypes; ++i) {
        switch ((Reduction::TYPE) i)
        {
        case Reduction::kFLOAT4: macros[1].Definition = "float4"; break;
        case Reduction::kFLOAT: macros[1].Definition = "float"; break;
        default: PRINTERROR("Reduction Type is invalid"); break;
        }
        //swprintf_s(temp, 32, L"Reduction<%S>_cs", macros[1].Definition);
        V(_Compile(L"Reduction_cs", macros, &reductionCS[i]));
        _cptReductionPSO[i].SetRootSignature(_rootsig);
        _cptReductionPSO[i].SetComputeShader(
            reductionCS[i]->GetBufferPointer(),
            reductionCS[i]->GetBufferSize());
        _cptReductionPSO[i].Finalize();
    }
}

void _CreateStaticResoure()
{
    _rootsig.Reset(3);
    _rootsig[0].InitAsConstants(0, 2);
    _rootsig[1].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0, 1);
    _rootsig[2].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 1);
    _rootsig.Finalize(L"Reduction",
        D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS);
    _CreatePSO();
}
}

Reduction::Reduction(TYPE type, uint32_t size, uint32_t bufCount)
    : _type(type), _size(size), _bufCount(bufCount)
{
    // To finish reduction in 2 passes, according to current config, total
    // element count should be smaller than 1024 * 1024
    ASSERT(size <= 0xfffff);
}

Reduction::~Reduction()
{
}

void
Reduction::OnCreateResource()
{
    std::call_once(_psoCompiled_flag, _CreateStaticResoure);
    uint32_t elementSize;
    switch (_type)
    {
    case kFLOAT4:
        elementSize = 4 * sizeof(float);
        _finalResult = new float[4 * _bufCount];
        break;
    case kFLOAT:
        elementSize = sizeof(float);
        _finalResult = new float[_bufCount];
        break;
    default:
        PRINTERROR("Reduction Type is invalid");break;
    }
    _intermmediateResultBuf.Create(
        L"Reduction", (_size + 1023) >> 10, elementSize);
    _finalResultBuf.Create(L"Reduction_result", _bufCount, elementSize);
    _readBackBuf.Create(L"Reduction_Readback", _bufCount, elementSize);
}

void
Reduction::OnDestory()
{
    switch (_type)
    {
    case kFLOAT4:
        delete[] (float*)_finalResult;
        break;
    case kFLOAT:
        delete[] (float*)_finalResult;
        break;
    default:
        PRINTERROR("Reduction Type is invalid"); break;
    }
    _intermmediateResultBuf.Destroy();
    _finalResultBuf.Destroy();
    _readBackBuf.Destroy();
}

void
Reduction::ProcessingOneBuffer(ComputeContext& cptCtx,
    StructuredBuffer* pInputBuf, uint32_t bufId)
{
    Trans(cptCtx, *pInputBuf, SRV);
    Trans(cptCtx, _intermmediateResultBuf, UAV);
    cptCtx.SetRootSignature(_rootsig);
    cptCtx.SetPipelineState(_cptReductionPSO[_type]);
    Bind(cptCtx, 2, 0, 1, &pInputBuf->GetSRV());
    UINT groupCount = (_size + 1023) >> 10;
    if (groupCount > 1) {
        Bind(cptCtx, 1, 0, 1, &_intermmediateResultBuf.GetUAV());
        cptCtx.SetConstants(0, DWParam(bufId), DWParam(0));
        cptCtx.Dispatch(groupCount);
        Trans(cptCtx, _intermmediateResultBuf, SRV);
        Bind(cptCtx, 2, 0, 1, &_intermmediateResultBuf.GetSRV());
    }
    // Even if I can do a 'recursive' loop to handle all size, but for larger
    // size it's better change the param in _cs.hlsl file
    Bind(cptCtx, 1, 0, 1, &_finalResultBuf.GetUAV());
    cptCtx.SetConstants(0, UINT(bufId), UINT(1));
    cptCtx.Dispatch(1);
}

void
Reduction::PrepareResult(ComputeContext& cptCtx)
{
    UINT elementSize;
    switch (_type)
    {
    case kFLOAT4: elementSize = 4 * sizeof(float); break;
    case kFLOAT: elementSize = sizeof(float); break;
    default: PRINTERROR("Reduction Type is invalid"); break;
    }
    cptCtx.CopyBufferRegion(
        _readBackBuf, 0, _finalResultBuf, 0, _bufCount * elementSize);
    _readBackFence = cptCtx.Flush(false);
}

void*
Reduction::ReadLastResult()
{
    Graphics::g_cmdListMngr.WaitForFence(_readBackFence);
    UINT elementSize;
    switch (_type)
    {
    case kFLOAT4: elementSize = 4 * sizeof(float); break;
    case kFLOAT: elementSize = sizeof(float); break;
    default: PRINTERROR("Reduction Type is invalid"); break;
    }
    D3D12_RANGE range = {0, elementSize};
    D3D12_RANGE umapRange = {};
    _readBackBuf.Map(&range, &_readBackPtr);
    memcpy(_finalResult, _readBackPtr, _bufCount * elementSize);
    _readBackBuf.Unmap(&umapRange);
    return _finalResult;
}

void
Reduction::RenderGui()
{
    using namespace ImGui;
    if (!CollapsingHeader("Reduction")) {
        return;
    }
    if (Button("ReconpileShaders##Reduction")) {
        _CreatePSO();
    }
}