#include "pch.h"
#include "SeparableFilter.h"
#define _USE_MATH_DEFINES
#include <math.h>

using namespace DirectX;
using namespace Microsoft::WRL;

#define Bind(ctx, rootIdx, offset, count, resource) \
ctx.SetDynamicDescriptors(rootIdx, offset, count, resource)

#define Trans(ctx, res, state) \
ctx.TransitionResource(res, state)

#define BeginTrans(ctx, res, state) \
ctx.BeginResourceTransition(res, state)

namespace {
enum EdgeRemoval {
    kKeepEdge = 0,
    kRemoveEdge = 1,
    kEdgeOption = 2
};
typedef D3D12_RESOURCE_STATES State;
const State CBV = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
const State UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
const State RTV = D3D12_RESOURCE_STATE_RENDER_TARGET;
const State psSRV = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
const State csSRV = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

GraphicsPSO _hPassPSO[kEdgeOption];
GraphicsPSO _vPassPSO[kEdgeOption];
RootSignature _rootSignature;

std::once_flag _psoCompiled_flag;

StructuredBuffer _gaussianWeightBuf;
DynAlloc* _gaussianWeightUploadBuf;

int _iKernelRadius = 3;
int _iEdgePixel = 2;
int _iEdgeThreshold = 100;
float _fRangeSigma = 25;
float _fGaussianWeight[64] = {};
bool _cbStaled = true;
bool _enabled = true;
bool _edgeRemoval = true;

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
    swprintf_s(fileName, 128, L"SeparableFilter_%ls.hlsl", shaderName);
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

float _GetGWeight(float offset, float sigma)
{
    float weight = (float)(1.f / (sqrt(2.f * M_PI) * sigma));
    return weight * exp(-(offset * offset) / (2.f * sigma * sigma));
}

void _UpdateGaussianWeightBuf(int iKernelRadius)
{
    float sigma = (iKernelRadius + 1) / 2.f;
    float sum = 0;
    for (int i = 0; i <= iKernelRadius; ++i) {
        float weight = _GetGWeight((float)i, sigma);
        _fGaussianWeight[i] = weight;
        sum += (i == 0) ? weight : 2 * weight;
    }
    for (int i = 0; i <= iKernelRadius; ++i) {
        _fGaussianWeight[i] /= sum;
    }
}

void _CreatePSOs()
{
    HRESULT hr;
    // Create Rootsignature
    _rootSignature.Reset(3);
    _rootSignature[0].InitAsConstantBuffer(0);
    _rootSignature[1].InitAsDescriptorRange(
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 2);
    _rootSignature[2].InitAsDescriptorRange(
        D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0, 1);
    _rootSignature.Finalize(L"SeparableFilter",
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS);

    // Create PSO
    ComPtr<ID3DBlob> quadVS;
    ComPtr<ID3DBlob> hPassPS[kEdgeOption];
    ComPtr<ID3DBlob> vPassPS[kEdgeOption];

    D3D_SHADER_MACRO macro[] = {
        {"__hlsl", "1"}, // 0
        {"HorizontalPass", "0"}, // 1
        {"EdgeRemoval", "0"}, // 2
        {nullptr, nullptr}
    };
    V(_Compile(L"SingleTriangleQuad_vs", macro, &quadVS));
    macro[1].Definition = "0";
    V(_Compile(L"BilateralFilter_ps", macro, &vPassPS[kKeepEdge]));
    macro[1].Definition = "1";
    V(_Compile(L"BilateralFilter_ps", macro, &hPassPS[kKeepEdge]));
    macro[2].Definition = "1";
    V(_Compile(L"BilateralFilter_ps", macro, &hPassPS[kRemoveEdge]));
    macro[1].Definition = "0";
    V(_Compile(L"BilateralFilter_ps", macro, &vPassPS[kRemoveEdge]));

    _hPassPSO[kKeepEdge].SetRootSignature(_rootSignature);
    _hPassPSO[kKeepEdge].SetRasterizerState(Graphics::g_RasterizerDefault);
    _hPassPSO[kKeepEdge].SetBlendState(Graphics::g_BlendDisable);
    _hPassPSO[kKeepEdge].SetDepthStencilState(Graphics::g_DepthStateDisabled);
    _hPassPSO[kKeepEdge].SetSampleMask(0xFFFFFFFF);
    _hPassPSO[kKeepEdge].SetInputLayout(0, nullptr);
    _hPassPSO[kKeepEdge].SetVertexShader(
        quadVS->GetBufferPointer(), quadVS->GetBufferSize());
    DXGI_FORMAT format = DXGI_FORMAT_R16_UINT;
    _hPassPSO[kKeepEdge].SetRenderTargetFormats(
        1, &format, DXGI_FORMAT_UNKNOWN);
    _hPassPSO[kKeepEdge].SetPrimitiveTopologyType(
        D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    _vPassPSO[kKeepEdge] = _hPassPSO[kKeepEdge];
    _vPassPSO[kRemoveEdge] = _vPassPSO[kKeepEdge];
    _vPassPSO[kRemoveEdge].SetPixelShader(
        vPassPS[kRemoveEdge]->GetBufferPointer(),
        vPassPS[kRemoveEdge]->GetBufferSize());
    _vPassPSO[kKeepEdge].SetPixelShader(
        vPassPS[kKeepEdge]->GetBufferPointer(),
        vPassPS[kKeepEdge]->GetBufferSize());
    _vPassPSO[kKeepEdge].Finalize();
    _vPassPSO[kRemoveEdge].Finalize();
    _hPassPSO[kRemoveEdge] = _hPassPSO[kKeepEdge];
    _hPassPSO[kKeepEdge].SetPixelShader(
        hPassPS[kKeepEdge]->GetBufferPointer(),
        hPassPS[kKeepEdge]->GetBufferSize());
    _hPassPSO[kRemoveEdge].SetPixelShader(
        hPassPS[kRemoveEdge]->GetBufferPointer(),
        hPassPS[kRemoveEdge]->GetBufferSize());
    _hPassPSO[kKeepEdge].Finalize();
    _hPassPSO[kRemoveEdge].Finalize();
}

void _CreateStaticResource(LinearAllocator& uploadHeapAlloc)
{
    _gaussianWeightBuf.Create(L"GaussianWeightsBuf", 64, sizeof(float));
    _gaussianWeightUploadBuf = new DynAlloc(
        std::move(uploadHeapAlloc.Allocate(sizeof(_fGaussianWeight))));
    _CreatePSOs();
    _UpdateGaussianWeightBuf(_iKernelRadius);
}
}

SeperableFilter::SeperableFilter()
{
    _dataCB.u2Reso = XMUINT2(0, 0);
}

SeperableFilter::~SeperableFilter()
{
}

HRESULT
SeperableFilter::OnCreateResoure(LinearAllocator& uploadHeapAlloc)
{
    HRESULT hr = S_OK;
    std::call_once(_psoCompiled_flag, _CreateStaticResource, uploadHeapAlloc);
    _gpuCB.Create(L"SeparableFilter_CB", 1, sizeof(CBuffer), (void*)&_dataCB);
    _pUploadCB = new DynAlloc(
        std::move(uploadHeapAlloc.Allocate(sizeof(CBuffer))));
    return hr;
}

void
SeperableFilter::OnDestory()
{
    delete _pUploadCB;
    _gpuCB.Destroy();
    _intermediateBuf.Destroy();
    _gaussianWeightBuf.Destroy();
}

void
SeperableFilter::OnRender(GraphicsContext& gfxCtx, const std::wstring procName,
    ColorBuffer* pInputTex, ColorBuffer* pOutputTex, ColorBuffer* pWeightTex)
{
    uint2 u2Reso = uint2(pOutputTex->GetWidth(), pOutputTex->GetHeight());
    ASSERT(pInputTex->GetWidth() == u2Reso.x);
    ASSERT(pInputTex->GetHeight() == u2Reso.y);
    if (u2Reso.x != _dataCB.u2Reso.x || u2Reso.y != _dataCB.u2Reso.y) {
        _Resize(u2Reso);
    }
    if (_cbStaled) {
        _UpdateCB(u2Reso, _fRangeSigma,
            _iKernelRadius, _iEdgeThreshold, _iEdgePixel);
        memcpy(_pUploadCB->DataPtr, &_dataCB, sizeof(CBuffer));
        gfxCtx.CopyBufferRegion(_gpuCB, 0, _pUploadCB->Buffer,
            _pUploadCB->Offset, sizeof(CBuffer));
        memcpy(_gaussianWeightUploadBuf->DataPtr, _fGaussianWeight,
            sizeof(_fGaussianWeight));
        gfxCtx.CopyBufferRegion(_gaussianWeightBuf, 0,
            _gaussianWeightUploadBuf->Buffer, _gaussianWeightUploadBuf->Offset,
            sizeof(_fGaussianWeight));
        Trans(gfxCtx, _gpuCB, CBV);
        Trans(gfxCtx, _gaussianWeightBuf, psSRV);
        _cbStaled = false;
    }
    if (!_enabled) {
        return;
    }
    Trans(gfxCtx, *pInputTex, psSRV | csSRV);
    Trans(gfxCtx, _intermediateBuf, RTV);
    Trans(gfxCtx, *pWeightTex, UAV);
    GPU_PROFILE(gfxCtx, procName.c_str());
    gfxCtx.SetRootSignature(_rootSignature);
    gfxCtx.SetPipelineState(_hPassPSO[_edgeRemoval]);
    gfxCtx.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    gfxCtx.SetRenderTargets(1, &_intermediateBuf.GetRTV());
    gfxCtx.SetViewport(_viewport);
    gfxCtx.SetScisor(_scisorRact);
    gfxCtx.SetConstantBuffer(0, _gpuCB.RootConstantBufferView());
    Bind(gfxCtx, 1, 0, 1, &pInputTex->GetSRV());
    Bind(gfxCtx, 1, 1, 1, &_gaussianWeightBuf.GetSRV());
    Bind(gfxCtx, 2, 0, 1, &pWeightTex->GetUAV());
    gfxCtx.Draw(3);
    Trans(gfxCtx, *pOutputTex, RTV);
    Trans(gfxCtx, _intermediateBuf, psSRV);
    Trans(gfxCtx, *pWeightTex, UAV);
    gfxCtx.SetPipelineState(_vPassPSO[_edgeRemoval]);
    gfxCtx.SetRenderTargets(1, &pOutputTex->GetRTV());
    Bind(gfxCtx, 1, 0, 1, &_intermediateBuf.GetSRV());
    Bind(gfxCtx, 1, 1, 1, &_gaussianWeightBuf.GetSRV());
    Bind(gfxCtx, 2, 0, 1, &pWeightTex->GetUAV());
    gfxCtx.Draw(3);
    BeginTrans(gfxCtx, _intermediateBuf, RTV);
}

bool
SeperableFilter::IsEnabled()
{
    return _enabled;
}

void
SeperableFilter::RenderGui()
{
    using namespace ImGui;
    if (!CollapsingHeader("BilateralFilter")) {
        return;
    }
    if (Button("RecompileShaders##SeperableFilter")) {
        _CreatePSOs();
    }
#define M(x) _cbStaled |= x
    M(Checkbox("Enable BilateralFilter", &_enabled)); SameLine();
    Checkbox("EdgeRemoval", &_edgeRemoval);
    if (_enabled) {
        M(SliderInt("Kernel Radius", (int*)&_iKernelRadius, 1, 32));
        _iEdgePixel = min(_iEdgePixel, _iKernelRadius);
        M(SliderFloat("Range Sigma", &_fRangeSigma, 5.f, 50.f));
        if (_edgeRemoval) {
            M(SliderInt(
                "Edge Pixel", &_iEdgePixel, 1, min(10, _iKernelRadius)));
            M(SliderInt("Edge Threshold", &_iEdgeThreshold, 50, 500, "%.0fmm"));
        }
    }
#undef  M
    if (_cbStaled) {
        _UpdateGaussianWeightBuf(_iKernelRadius);
    }
}

void
SeperableFilter::_Resize(DirectX::XMUINT2 reso)
{
    if (_dataCB.u2Reso.x != reso.x || _dataCB.u2Reso.y != reso.y) {
        _intermediateBuf.Destroy();
        _intermediateBuf.Create(L"BilateralTemp",
            (uint32_t)reso.x, (uint32_t)reso.y, 1, DXGI_FORMAT_R16_UINT);
        _viewport.Width = static_cast<float>(reso.x);
        _viewport.Height = static_cast<float>(reso.y);
        _viewport.MaxDepth = 1.0;
        _scisorRact.right = static_cast<LONG>(reso.x);
        _scisorRact.bottom = static_cast<LONG>(reso.y);

        // We set 50mm as edge threshold, so 50 will be 2*deviation
        // ->deviation = 25;
        // ->variance = 625
        _UpdateCB(reso, _fRangeSigma, _iKernelRadius,
            _iEdgeThreshold, _iEdgePixel);
        _cbStaled = true;
    }
}

void
SeperableFilter::_UpdateCB(uint2 u2Reso, float fRangeSigma, int iKernelRadius,
    int iEdgeThreshold, int iEdgePixel)
{
    _dataCB.u2Reso = u2Reso;
    _dataCB.fRangeVar = fRangeSigma * fRangeSigma;
    _dataCB.iKernelRadius = iKernelRadius;
    _dataCB.iEdgePixel = iEdgePixel;
    _dataCB.iEdgeThreshold = iEdgeThreshold;
}