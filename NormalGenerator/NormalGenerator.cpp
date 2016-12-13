#include "pch.h"
#include "NormalGenerator.h"

using namespace DirectX;
using namespace Microsoft::WRL;

#define Bind(ctx, rootIdx, offset, count, resource) \
ctx.SetDynamicDescriptors(rootIdx, offset, count, resource)

#define BindCB(ctx, rootIdx, size, ptr) \
ctx.SetDynamicConstantBufferView(rootIdx, size, ptr)

namespace {
    const D3D12_RESOURCE_STATES UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    const D3D12_RESOURCE_STATES SRV =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    const DXGI_FORMAT _normalMapFormat = DXGI_FORMAT_R10G10B10A2_UNORM;

    RootSignature _rootsig;
    ComputePSO _cptGetNormalPSO;
    std::once_flag _psoCompiled_flag;

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
            D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS|
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
}

NormalGenerator::~NormalGenerator()
{
}

void
NormalGenerator::OnCreateResource()
{
    std::call_once(_psoCompiled_flag, _CreateStaticResource);
    _normalMap.Create(_name.c_str(),
        _dataCB.u2Reso.x, _dataCB.u2Reso.y, 1, _normalMapFormat);
}

void
NormalGenerator::OnDestory()
{
    _normalMap.Destroy();
}

void
NormalGenerator::OnResize(uint2 reso)
{
    _dataCB.u2Reso = reso;
    _normalMap.Destroy();
    _normalMap.Create(_name.c_str(), reso.x, reso.y, 1, _normalMapFormat);
}

void
NormalGenerator::OnProcessing(
    ComputeContext& cptCtx, ColorBuffer* pInputTex)
{
    GPU_PROFILE(cptCtx, _name.c_str());
    cptCtx.SetRootSignature(_rootsig);
    cptCtx.SetPipelineState(_cptGetNormalPSO);
    cptCtx.TransitionResource(*pInputTex, UAV);
    BindCB(cptCtx, 0, sizeof(_dataCB), (void*)&_dataCB);
    Bind(cptCtx, 1, 0, 1, &_normalMap.GetUAV());
    Bind(cptCtx, 2, 0, 1, &pInputTex->GetSRV());
    cptCtx.Dispatch2D(_dataCB.u2Reso.x, _dataCB.u2Reso.y);
    cptCtx.BeginResourceTransition(*pInputTex, SRV);
}

void
NormalGenerator::RenderGui()
{
    if (!ImGui::CollapsingHeader("NormalGenerator Settings")) {
        return;
    }
    if (ImGui::Button("Recompile Shaders")) {
        _CreatePSO();
    }
    ImGui::SliderFloat("Dist Threshold", &_dataCB.fDistThreshold, 0.05f, 0.5f);
}

ColorBuffer*
NormalGenerator::GetNormalMap()
{
    return &_normalMap;
}