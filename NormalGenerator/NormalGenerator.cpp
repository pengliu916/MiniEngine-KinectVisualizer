#include "pch.h"
#include "NormalGenerator.h"

using namespace DirectX;
using namespace Microsoft::WRL;

#define Trans(ctx, res, state) \
ctx.TransitionResource(res, state)

#define Bind(ctx, rootIdx, offset, count, resource) \
ctx.SetDynamicDescriptors(rootIdx, offset, count, resource)

namespace {
enum OutMode {
    kWithoutConfidence = 0,
    kWithConfidence = 1,
    kOutMode = 2,
};

typedef D3D12_RESOURCE_STATES State;
const State UAV   = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
const State CBV   = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
const State psSRV = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
const State csSRV = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

D3D12_CPU_DESCRIPTOR_HANDLE _nullUAVDescrptor;

RootSignature _rootsig;
ComputePSO _cptGetNormalPSO[kOutMode];
std::once_flag _psoCompiled_flag;
bool _bOutputConfidence = true;

// When the angle between surface normal and view direction is larger than
// acos(voxelSize/truncDist), voxel on each size of surface may not contain
// opposite sign, thus we will miss surface from some point of view
const float _fMinAngleThreshold = 0.5359f;
float _fAngleThreshold = _fMinAngleThreshold;
float _fDistThreshold = 0.1f;
bool _scbStaled = true;
bool _typedLoadSupported = false;

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
    swprintf_s(fileName, 128, L"NormalGenerator_%ls.hlsl", shaderName);
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
    HRESULT hr;
    ComPtr<ID3DBlob> getNormalCS[kOutMode];
    D3D_SHADER_MACRO macros[] = {
        {"__hlsl", "1"},
        {"CONFIDENCE_OUT", "1"},
        {"NO_TYPED_LOAD", "1"},
        {nullptr, nullptr}
    };
    if (_typedLoadSupported) {
        macros[2].Definition = "0";
    }
    V(_Compile(L"GetNormal_cs", macros, &getNormalCS[kWithConfidence]));
    macros[1].Definition = "0";
    V(_Compile(L"GetNormal_cs", macros, &getNormalCS[kWithoutConfidence]));

    _cptGetNormalPSO[kWithConfidence].SetRootSignature(_rootsig);
    _cptGetNormalPSO[kWithConfidence].SetComputeShader(
        getNormalCS[kWithConfidence]->GetBufferPointer(),
        getNormalCS[kWithConfidence]->GetBufferSize());
    _cptGetNormalPSO[kWithConfidence].Finalize();
    _cptGetNormalPSO[kWithoutConfidence].SetRootSignature(_rootsig);
    _cptGetNormalPSO[kWithoutConfidence].SetComputeShader(
        getNormalCS[kWithoutConfidence]->GetBufferPointer(),
        getNormalCS[kWithoutConfidence]->GetBufferSize());
    _cptGetNormalPSO[kWithoutConfidence].Finalize();
}

void _CreateStaticResource()
{
    HRESULT hr;
    // Feature support checking
    D3D12_FEATURE_DATA_D3D12_OPTIONS FeatureData = {};
    V(Graphics::g_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS,
        &FeatureData, sizeof(FeatureData)));
    if (SUCCEEDED(hr)) {
        if (FeatureData.TypedUAVLoadAdditionalFormats) {
            D3D12_FEATURE_DATA_FORMAT_SUPPORT FormatSupport =
            {DXGI_FORMAT_R8_SNORM, D3D12_FORMAT_SUPPORT1_NONE,
                D3D12_FORMAT_SUPPORT2_NONE};
            V(Graphics::g_device->CheckFeatureSupport(
                D3D12_FEATURE_FORMAT_SUPPORT, &FormatSupport,
                sizeof(FormatSupport)));
            if (FAILED(hr)) {
                PRINTERROR("Checking Feature Support Failed");
            }
            if ((FormatSupport.Support2 &
                D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) != 0) {
                _typedLoadSupported = true;
                PRINTINFO("Typed load is supported");
            } else {
                PRINTWARN("Typed load is not supported");
            }
        } else {
            PRINTWARN("TypedLoad AdditionalFormats load is not supported");
        }
    }
    // Create nulldescriptor
    _nullUAVDescrptor = Graphics::g_pCSUDescriptorHeap->Append().GetCPUHandle();
    D3D12_UNORDERED_ACCESS_VIEW_DESC UAVDesc = {};
    UAVDesc.Format = DXGI_FORMAT_R8_UNORM;
    UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    Graphics::g_device->CreateUnorderedAccessView(
        nullptr, nullptr, &UAVDesc, _nullUAVDescrptor);

    // Create RootSignature
    _rootsig.Reset(4, 1);
    _rootsig.InitStaticSampler(0, Graphics::g_SamplerLinearClampDesc);
    _rootsig[0].InitAsConstants(0, 2);
    _rootsig[1].InitAsConstantBuffer(1);
    _rootsig[2].InitAsDescriptorRange(
        D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0, 2);
    _rootsig[3].InitAsDescriptorRange(
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 2);
    _rootsig.Finalize(L"NormalGenerator",
        D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS);
    _CreatePSO();
}
}

NormalGenerator::NormalGenerator()
{
    _dataCB.fAngleThreshold = _fAngleThreshold;
    _dataCB.fDistThreshold = _fDistThreshold;
}

NormalGenerator::~NormalGenerator()
{
}

void
NormalGenerator::OnCreateResource(LinearAllocator& uploadHeapAlloc)
{
    std::call_once(_psoCompiled_flag, _CreateStaticResource);
    _gpuCB.Create(L"NormalGenerator_CB", 1, sizeof(CBuffer),
        (void*)&_dataCB);
    _pUploadCB = new DynAlloc(
        std::move(uploadHeapAlloc.Allocate(sizeof(CBuffer))));
}

void
NormalGenerator::OnDestory()
{
    delete _pUploadCB;
    _gpuCB.Destroy();
}

void
NormalGenerator::OnProcessing(ComputeContext& cptCtx,
    const std::wstring& procName, ColorBuffer* pInputTex,
    ColorBuffer* pOutputTex, ColorBuffer* pConfidenceTex)
{
    ASSERT(pOutputTex->GetWidth() == pInputTex->GetWidth());
    ASSERT(pOutputTex->GetHeight() == pInputTex->GetHeight());

    if (_scbStaled) {
        _dataCB.fAngleThreshold = _fAngleThreshold;
        _dataCB.fDistThreshold = _fDistThreshold;
        memcpy(_pUploadCB->DataPtr, &_dataCB, sizeof(CBuffer));
        cptCtx.CopyBufferRegion(_gpuCB, 0, _pUploadCB->Buffer,
            _pUploadCB->Offset, sizeof(CBuffer));
        Trans(cptCtx, _gpuCB, CBV);
        _scbStaled = false;
    }
    Trans(cptCtx, *pInputTex, csSRV);
    Trans(cptCtx, *pOutputTex, UAV);
    GPU_PROFILE(cptCtx, procName.c_str());
    cptCtx.SetRootSignature(_rootsig);
    if (pConfidenceTex) {
        if (_bOutputConfidence) {
            Trans(cptCtx, *pConfidenceTex, UAV);
            Bind(cptCtx, 2, 1, 1, &pConfidenceTex->GetUAV());
            if (!_typedLoadSupported) {
                Bind(cptCtx, 3, 1, 1, &pConfidenceTex->GetSRV());
            }
            cptCtx.SetPipelineState(_cptGetNormalPSO[kWithConfidence]);
        } else {
            cptCtx.SetPipelineState(_cptGetNormalPSO[kWithoutConfidence]);
        }
    } else {
        Bind(cptCtx, 2, 1, 1, &_nullUAVDescrptor);
        cptCtx.SetPipelineState(_cptGetNormalPSO[kWithoutConfidence]);
    }
    cptCtx.SetConstants(0, DWParam(1.f / pOutputTex->GetWidth()),
        DWParam(1.f / pOutputTex->GetHeight()));
    cptCtx.SetConstantBuffer(1, _gpuCB.RootConstantBufferView());
    Bind(cptCtx, 2, 0, 1, &pOutputTex->GetUAV());
    Bind(cptCtx, 3, 0, 1, &pInputTex->GetSRV());
    cptCtx.Dispatch2D(pOutputTex->GetWidth(), pOutputTex->GetHeight());
}

void
NormalGenerator::RenderGui()
{
    using namespace ImGui;
    if (!CollapsingHeader("NormalGenerator")) {
        return;
    }
    if (Button("RecompileShaders##NormalGenerator")) {
        _CreatePSO();
    }
    Checkbox("Output Confidence", &_bOutputConfidence);
#define M(x) _scbStaled |= x
    M(SliderFloat("Angle Threshold",
        &_fAngleThreshold, _fMinAngleThreshold, 0.9f));
    M(SliderFloat("Dist Threshold", &_fDistThreshold, 0.05f, 0.5f, "%.2fm"));
#undef M
}