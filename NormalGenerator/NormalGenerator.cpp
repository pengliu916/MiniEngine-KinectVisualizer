#include "pch.h"
#include "NormalGenerator.h"

using namespace DirectX;
using namespace Microsoft::WRL;

#define Trans(ctx, res, state) \
ctx.TransitionResource(res, state)

#define Bind(ctx, rootIdx, offset, count, resource) \
ctx.SetDynamicDescriptors(rootIdx, offset, count, resource)

namespace {
typedef D3D12_RESOURCE_STATES State;
const State UAV   = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
const State psSRV = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
const State csSRV = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

const DXGI_FORMAT _normalMapFormat = DXGI_FORMAT_R10G10B10A2_UNORM;

RootSignature _rootsig;
ComputePSO _cptGetNormalPSO;
std::once_flag _psoCompiled_flag;

float _fDistThreshold = FLT_MAX;
bool _cbStaled = true;

void _CreatePSO()
{
    HRESULT hr;
    ComPtr<ID3DBlob> getNormalCS;
    D3D_SHADER_MACRO macros[] = {
        {"__hlsl", "1"},
        {nullptr, nullptr}
    };
    V(Graphics::CompileShaderFromFile(Core::GetAssetFullPath(
        L"NormalGenerator_GetNormal_cs.hlsl").c_str(), macros,
        D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "cs_5_1",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &getNormalCS));
    _cptGetNormalPSO.SetRootSignature(_rootsig);
    _cptGetNormalPSO.SetComputeShader(
        getNormalCS->GetBufferPointer(), getNormalCS->GetBufferSize());
    _cptGetNormalPSO.Finalize();
}

void _CreateStaticResource()
{
    // Create RootSignature
    _rootsig.Reset(3, 1);
    _rootsig.InitStaticSampler(0, Graphics::g_SamplerLinearClampDesc);
    _rootsig[0].InitAsConstantBuffer(0);
    _rootsig[1].InitAsDescriptorRange(
        D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0, 1);
    _rootsig[2].InitAsDescriptorRange(
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 1);
    _rootsig.Finalize(L"NormalGenerator",
        D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS);

    _CreatePSO();
}
}

NormalGenerator::NormalGenerator(uint2 reso, const std::wstring& name)
    : _name(name)
{
    _dataCB.u2Reso = reso;
    _dataCB.fDistThreshold = 0.1f;
}

NormalGenerator::~NormalGenerator()
{
}

void
NormalGenerator::OnCreateResource(LinearAllocator& uploadHeapAlloc)
{
    std::call_once(_psoCompiled_flag, _CreateStaticResource);
    _normalMap.Create(_name.c_str(),
        _dataCB.u2Reso.x, _dataCB.u2Reso.y, 1, _normalMapFormat);
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
    _normalMap.Destroy();
}

void
NormalGenerator::OnResize(uint2 reso)
{
    _cbStaled = true;
    _dataCB.u2Reso = reso;
    _normalMap.Destroy();
    _normalMap.Create(_name.c_str(), reso.x, reso.y, 1, _normalMapFormat);
}

void
NormalGenerator::OnProcessing(
    ComputeContext& cptCtx, ColorBuffer* pInputTex)
{
    if (_cbStaled) {
        _dataCB.fDistThreshold = _fDistThreshold;
        memcpy(_pUploadCB->DataPtr, &_dataCB, sizeof(CBuffer));
        cptCtx.CopyBufferRegion(_gpuCB, 0, _pUploadCB->Buffer,
            _pUploadCB->Offset, sizeof(CBuffer));
        _cbStaled = false;
    }
    Trans(cptCtx, *pInputTex, csSRV);
    GPU_PROFILE(cptCtx, _name.c_str());
    cptCtx.SetPipelineState(_cptGetNormalPSO);
    cptCtx.SetRootSignature(_rootsig);
    cptCtx.TransitionResource(*pInputTex, UAV);
    cptCtx.SetConstantBuffer(0, _gpuCB.RootConstantBufferView());
    Bind(cptCtx, 1, 0, 1, &_normalMap.GetUAV());
    Bind(cptCtx, 2, 0, 1, &pInputTex->GetSRV());
    cptCtx.Dispatch2D(_dataCB.u2Reso.x, _dataCB.u2Reso.y);
}

void
NormalGenerator::RenderGui()
{
    using namespace ImGui;
    if (!CollapsingHeader("NormalGenerator Settings")) {
        return;
    }
    if (Button("Recompile Shaders")) {
        _CreatePSO();
    }
    _cbStaled = SliderFloat("Dist Threshold", &_fDistThreshold, 0.05f, 0.5f);
}

ColorBuffer*
NormalGenerator::GetNormalMap()
{
    return &_normalMap;
}