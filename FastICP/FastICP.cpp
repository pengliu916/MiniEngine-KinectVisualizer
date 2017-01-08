#include "pch.h"
#include "FastICP.h"
#include "Reduction/Reduction.h"

#define Trans(ctx, res, state) \
ctx.TransitionResource(res, state)

#define Bind(ctx, rootIdx, offset, count, resource) \
ctx.SetDynamicDescriptors(rootIdx, offset, count, resource)

namespace {
typedef D3D12_RESOURCE_STATES State;
const State UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
const State SRV = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

RootSignature _rootsig;
ComputePSO _cptPreparePSO;

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
    swprintf_s(fileName, 128, L"FastICP_%ls.hlsl", shaderName);
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
    ComPtr<ID3DBlob> prepareCS;
    D3D_SHADER_MACRO macros[] = {
        {"__hlsl", "1"},
        {nullptr, nullptr}
    };
    V(_Compile(L"Prepare_cs", macros, &prepareCS));

#define CreatePSO( ObjName, Shader)\
ObjName.SetRootSignature(_rootsig);\
ObjName.SetComputeShader(Shader->GetBufferPointer(), Shader->GetBufferSize());\
ObjName.Finalize();
    CreatePSO(_cptPreparePSO, prepareCS);
#undef  CreatePSO
}

void _CreateStaticResource()
{
    // Create RootSignature
    _rootsig.Reset(3);
    _rootsig[0].InitAsConstantBuffer(0);
    _rootsig[1].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0, 7);
    _rootsig[2].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 5);
    _rootsig.Finalize(L"FastICP",
        D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS);
    _CreatePSO();
}
}

FastICP::FastICP()
{
    _dataCB.u2AlignedReso = uint2(0, 0);
    _dataCB.fNormalDiffThreshold = 0.3f;
}

FastICP::~FastICP()
{
    if (_pReductionExec) {
        delete _pReductionExec;
        _pReductionExec = nullptr;
    }
}

void
FastICP::OnCreateResource(uint2 inputReso)
{
    std::call_once(_psoCompiled_flag, _CreateStaticResource);
    uint16_t w = inputReso.x;
    uint16_t h = inputReso.y;
    w = (w + CPU_THREAD_X - 1) & ~(CPU_THREAD_X - 1);
    h = (h + CPU_THREAD_Y - 1) & ~(CPU_THREAD_Y - 1);
    if (_dataCB.u2AlignedReso.x != w || _dataCB.u2AlignedReso.y != h) {
        _dataCB.u2AlignedReso = uint2(w, h);
        _CreatePrepareBuffers(uint2(w, h));
    }
}

void
FastICP::OnDestory()
{
    _pReductionExec->OnDestory();
    for (uint8_t i = 0; i < DATABUF_COUNT; ++i) {
        _dataPackBuf[i].Destroy();
    }
}

void
FastICP::OnProcessing(ComputeContext& cptCtx, uint8_t iteration,
    ColorBuffer* pWeight,
    ColorBuffer* pTSDFDepth, ColorBuffer* pTSDFNormal,
    ColorBuffer* pKinectDepth, ColorBuffer* pKinectNormal)
{
    wchar_t profilerName[32];
    uint16_t w = pWeight->GetWidth();
    uint16_t h = pWeight->GetHeight();
    ASSERT(w == pTSDFDepth->GetWidth() && h == pTSDFDepth->GetHeight());
    ASSERT(w == pTSDFNormal->GetWidth() && h == pTSDFNormal->GetHeight());
    ASSERT(w == pKinectDepth->GetWidth() && h == pKinectDepth->GetHeight());
    ASSERT(w == pKinectNormal->GetWidth() && h == pKinectNormal->GetHeight());

    w = (w + CPU_THREAD_X - 1) & ~(CPU_THREAD_X - 1);
    h = (h + CPU_THREAD_Y - 1) & ~(CPU_THREAD_Y - 1);
    if (_dataCB.u2AlignedReso.x != w || _dataCB.u2AlignedReso.y != h) {
        _dataCB.u2AlignedReso = uint2(w, h);
        _CreatePrepareBuffers(uint2(w, h));
    }
    Trans(cptCtx, *pWeight, SRV);
    Trans(cptCtx, *pTSDFDepth, SRV);
    Trans(cptCtx, *pTSDFNormal, SRV);
    Trans(cptCtx, *pKinectDepth, SRV);
    Trans(cptCtx, *pKinectNormal, SRV);
    cptCtx.SetRootSignature(_rootsig);
    cptCtx.SetDynamicConstantBufferView(0, sizeof(CBuffer), (void*)&_dataCB);
    {
        swprintf_s(profilerName, 32, L"ICP_Prepare[%d]", iteration);
        GPU_PROFILE(cptCtx, profilerName);
        cptCtx.SetPipelineState(_cptPreparePSO);
        Bind(cptCtx, 2, 0, 1, &pKinectDepth->GetSRV());
        Bind(cptCtx, 2, 1, 1, &pTSDFDepth->GetSRV());
        Bind(cptCtx, 2, 2, 1, &pKinectNormal->GetSRV());
        Bind(cptCtx, 2, 3, 1, &pTSDFNormal->GetSRV());
        Bind(cptCtx, 2, 4, 1, &pWeight->GetSRV());
        for (int i = 0; i < DATABUF_COUNT; ++i) {
            Bind(cptCtx, 1, i, 1, &_dataPackBuf[i].GetUAV());
        }
        cptCtx.Dispatch2D(w, h);
    }
    {
        swprintf_s(profilerName, 32, L"ICP_Reduction[%d]", iteration);
        GPU_PROFILE(cptCtx, profilerName);
        for (int i = 0; i < DATABUF_COUNT; ++i) {
            _pReductionExec->ProcessingOneBuffer(cptCtx, &_dataPackBuf[i], i);
        }
    }
    _pReductionExec->PrepareResult(cptCtx);
}

void
FastICP::OnSolving()
{
    _pReductionExec->ReadLastResult(_reductionResult);
}

void
FastICP::RenderGui()
{
    using namespace ImGui;
    if (!CollapsingHeader("Debug ICP")) {
        return;
    }
    if (Button("ReconpileShaders##FastICP")) {
        _CreatePSO();
    }
    for (int i = 0; i < DATABUF_COUNT; ++i) {
        Text("Buf[%d]:%8.1f,%8.1f,%8.1f,%8.1f", i,
            _reductionResult[i * 4],
            _reductionResult[i * 4 + 1],
            _reductionResult[i * 4 + 2],
            _reductionResult[i * 4 + 3]);
    }
}

void
FastICP::_CreatePrepareBuffers(uint2 u2Reso)
{
    uint32_t count = u2Reso.x * u2Reso.y;
    wchar_t temp[32];
    for (uint8_t i = 0; i < DATABUF_COUNT; ++i) {
        _dataPackBuf[i].Destroy();
        swprintf_s(temp, 32, L"DataPackBuf[%d]", i);
        _dataPackBuf[i].Create(temp, count, 4 * sizeof(float));
        if (_pReductionExec) {
            _pReductionExec->OnDestory();
            delete _pReductionExec;
        }
        _pReductionExec =
            new Reduction(Reduction::kFLOAT4, count, DATABUF_COUNT);
        _pReductionExec->OnCreateResource();
    }
}