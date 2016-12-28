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

#define TransFlush(ctx, res, state) \
ctx.TransitionResource(res, state, true)

#define BeginTrans(ctx, res, state) \
ctx.BeginResourceTransition(res, state)

namespace {
typedef D3D12_RESOURCE_STATES State;
const State CBV = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
const State RTV = D3D12_RESOURCE_STATE_RENDER_TARGET;
const State psSRV = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
const State csSRV = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

GraphicsPSO _hPassPSO;
GraphicsPSO _vPassPSO;
RootSignature _rootSignature;

std::once_flag _psoCompiled_flag;

StructuredBuffer _gaussianWeightBuf;
DynAlloc* _gaussianWeightUploadBuf;

int _iKernelRadius = 3;
float _fRangeVar = 625;
float _fGaussianWeight[64] = {};
bool _cbStaled = true;
bool _enabled = false;

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
    _rootSignature.Reset(2);
    _rootSignature[0].InitAsConstantBuffer(0);
    _rootSignature[1].InitAsDescriptorRange(
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 2);
    _rootSignature.Finalize(L"SeparableFilter",
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS);

    // Create PSO
    ComPtr<ID3DBlob> quadVS;
    ComPtr<ID3DBlob> hPassPS;
    ComPtr<ID3DBlob> vPassPS;

    uint32_t compileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
#if defined(DEBUG) || defined(_DEBUG)
    compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    D3D_SHADER_MACRO macro[] = {
        {"__hlsl", "1"}, // 0
        {"HorizontalPass", "0"}, // 1
        {nullptr, nullptr}
    };
    V(Graphics::CompileShaderFromFile(Core::GetAssetFullPath(
        L"SeparableFilter_SingleTriangleQuad_vs.hlsl").c_str(), macro,
        D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "vs_5_1",
        compileFlags, 0, &quadVS));
    macro[1].Definition = "0";
    V(Graphics::CompileShaderFromFile(Core::GetAssetFullPath(
        L"SeparableFilter_BilateralFilter_ps.hlsl").c_str(), macro,
        D3D_COMPILE_STANDARD_FILE_INCLUDE, "main",
        "ps_5_1", compileFlags, 0, &vPassPS));
    macro[1].Definition = "1";
    V(Graphics::CompileShaderFromFile(Core::GetAssetFullPath(
        L"SeparableFilter_BilateralFilter_ps.hlsl").c_str(), macro,
        D3D_COMPILE_STANDARD_FILE_INCLUDE, "main",
        "ps_5_1", compileFlags, 0, &hPassPS));

    _hPassPSO.SetRootSignature(_rootSignature);
    _hPassPSO.SetRasterizerState(Graphics::g_RasterizerDefault);
    _hPassPSO.SetBlendState(Graphics::g_BlendDisable);
    _hPassPSO.SetDepthStencilState(Graphics::g_DepthStateDisabled);
    _hPassPSO.SetSampleMask(0xFFFFFFFF);
    _hPassPSO.SetInputLayout(0, nullptr);
    _hPassPSO.SetVertexShader(
        quadVS->GetBufferPointer(), quadVS->GetBufferSize());
    DXGI_FORMAT format = DXGI_FORMAT_R16_UINT;
    _hPassPSO.SetRenderTargetFormats(
        1, &format, DXGI_FORMAT_UNKNOWN);
    _hPassPSO.SetPrimitiveTopologyType(
        D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    _vPassPSO = _hPassPSO;
    _vPassPSO.SetPixelShader(
        vPassPS->GetBufferPointer(), vPassPS->GetBufferSize());
    _vPassPSO.Finalize();
    _hPassPSO.SetPixelShader(
        hPassPS->GetBufferPointer(), hPassPS->GetBufferSize());
    _hPassPSO.Finalize();
}

void _CreateStaticResource(LinearAllocator& uploadHeapAlloc)
{
    _gaussianWeightBuf.Create(L"GaussianWeightsBuf", 64, sizeof(float));
    _gaussianWeightUploadBuf = new DynAlloc(
        std::move(uploadHeapAlloc.Allocate(sizeof(_fGaussianWeight))));
    _CreatePSOs();
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
    _gpuCB.Create(L"SeparableFilter_CB", 1, sizeof(CBuffer),
        (void*)&_dataCB);
    _pUploadCB = new DynAlloc(
        std::move(uploadHeapAlloc.Allocate(sizeof(CBuffer))));
    return hr;
}

void
SeperableFilter::OnDestory()
{
    delete _pUploadCB;
    _gpuCB.Destroy();
    _filteredBuf.Destroy();
    _intermediateBuf.Destroy();
    _gaussianWeightBuf.Destroy();
}

void
SeperableFilter::OnResize(DirectX::XMUINT2 reso)
{
    if (_dataCB.u2Reso.x != reso.x || _dataCB.u2Reso.y != reso.y) {
        _intermediateBuf.Destroy();
        _intermediateBuf.Create(L"BilateralTemp",
            (uint32_t)reso.x, (uint32_t)reso.y, 1, DXGI_FORMAT_R16_UINT);
        _filteredBuf.Destroy();
        _filteredBuf.Create(L"FilteredlOut",
            (uint32_t)reso.x, (uint32_t)reso.y, 1, DXGI_FORMAT_R16_UINT);
        _viewport.Width = static_cast<float>(reso.x);
        _viewport.Height = static_cast<float>(reso.y);
        _viewport.MaxDepth = 1.0;
        _scisorRact.right = static_cast<LONG>(reso.x);
        _scisorRact.bottom = static_cast<LONG>(reso.y);

        // We set 50mm as edge threshold, so 50 will be 2*deviation
        // ->deviation = 25;
        // ->variance = 625
        _UpdateCB(reso, _fRangeVar, _iKernelRadius);
        _cbStaled = true;
    }
}

void
SeperableFilter::OnRender(
    GraphicsContext& gfxCtx, ColorBuffer* pInputTex)
{
    if (!_enabled) {
        return;
    }
    if (_cbStaled) {
        _UpdateCB(_dataCB.u2Reso, _fRangeVar, _iKernelRadius);
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
    Trans(gfxCtx, *pInputTex, psSRV | csSRV);
    Trans(gfxCtx, _intermediateBuf, RTV);
    BeginTrans(gfxCtx, _filteredBuf, RTV);
    GPU_PROFILE(gfxCtx, L"BilateralFilter");
    gfxCtx.SetRootSignature(_rootSignature);
    gfxCtx.SetPipelineState(_hPassPSO);
    gfxCtx.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    gfxCtx.SetRenderTargets(1, &_intermediateBuf.GetRTV());
    gfxCtx.SetViewport(_viewport);
    gfxCtx.SetScisor(_scisorRact);
    gfxCtx.SetConstantBuffer(0, _gpuCB.RootConstantBufferView());
    Bind(gfxCtx, 1, 0, 1, &pInputTex->GetSRV());
    Bind(gfxCtx, 1, 1, 1, &_gaussianWeightBuf.GetSRV());
    gfxCtx.Draw(3);
    Trans(gfxCtx, _filteredBuf, RTV);
    Trans(gfxCtx, _intermediateBuf, psSRV);
    gfxCtx.SetPipelineState(_vPassPSO);
    gfxCtx.SetRenderTargets(1, &_filteredBuf.GetRTV());
    Bind(gfxCtx, 1, 0, 1, &_intermediateBuf.GetSRV());
    Bind(gfxCtx, 1, 1, 1, &_gaussianWeightBuf.GetSRV());
    gfxCtx.Draw(3);
    BeginTrans(gfxCtx, _intermediateBuf, RTV);
}

void
SeperableFilter::RenderGui()
{
    using namespace ImGui;
    if (!CollapsingHeader("BilateralFilter")) {
        return;
    }
    if (Button("RecompileShaders##NormalGenerator")) {
        _CreatePSOs();
    }
    _cbStaled |= Checkbox("Enable Bilateral Filter", &_enabled);
    _cbStaled |= SliderInt("Kernel Radius", (int*)&_iKernelRadius, 1, 32);
    _cbStaled |= DragFloat("Range Var", &_fRangeVar, 1.f, 100, 2500);
    if (_cbStaled) {
        _UpdateGaussianWeightBuf(_iKernelRadius);
    }
}

ColorBuffer*
SeperableFilter::GetFilteredTex()
{
    return _enabled ? &_filteredBuf : nullptr;
}


void
SeperableFilter::_UpdateCB(uint2 u2Reso, float fRangeVar, int iKernelRadius)
{
    _dataCB.u2Reso = u2Reso;
    _dataCB.fRangeVar = fRangeVar;
    if (_dataCB.iKernelRadius == iKernelRadius) {
        return;
    }
    _dataCB.iKernelRadius = iKernelRadius;
}