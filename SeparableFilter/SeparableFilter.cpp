#include "pch.h"
#include "SeparableFilter.h"

using namespace DirectX;
using namespace Microsoft::WRL;

namespace {
    GraphicsPSO _hPassPSO[SeperableFilter::kNumKernelDiameter];
    GraphicsPSO _vPassPSO[SeperableFilter::kNumKernelDiameter];
    RootSignature _rootSignature;
}

SeperableFilter::SeperableFilter()
{
    _dataCB.u2Reso = XMUINT2(0, 0);
    // We set 50mm as edge threshold, so 50 will be 2*deviation
    // ->deviation = 25;
    // ->variance = 625
    _dataCB.fGaussianVar = 625;
}

SeperableFilter::~SeperableFilter()
{
}

HRESULT SeperableFilter::OnCreateResoure(DXGI_FORMAT bufferFormat)
{
    HRESULT hr = S_OK;
    _outTexFormat = bufferFormat;
    // Create Rootsignature
    _rootSignature.Reset(2);
    _rootSignature[0].InitAsConstantBuffer(0);
    _rootSignature[1].InitAsDescriptorRange(
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 1);
    _rootSignature.Finalize(L"SeparableFilter",
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS);

    // Create PSO
    ComPtr<ID3DBlob> quadVS;
    ComPtr<ID3DBlob> hPassPS[kNumKernelDiameter];
    ComPtr<ID3DBlob> vPassPS[kNumKernelDiameter];

    uint32_t compileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    D3D_SHADER_MACRO macro[] = {
        {"__hlsl", "1"}, // 0
        {"HorizontalPass", "0"}, // 1
        {"KERNEL_RADIUS", "0"}, // 2
        {nullptr, nullptr}
    };
    V(Graphics::CompileShaderFromFile(Core::GetAssetFullPath(
        L"SeparableFilter_SingleTriangleQuad_vs.hlsl").c_str(), macro, 
        D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "vs_5_1", 
        compileFlags, 0, &quadVS));
    char kernelSizeStr[8];
    for (int i = 0; i < kNumKernelDiameter; ++i) {
        sprintf_s(kernelSizeStr, 8, "%d", i);
        macro[2].Definition = kernelSizeStr;
        macro[1].Definition = "0";
        V(Graphics::CompileShaderFromFile(Core::GetAssetFullPath(
            L"SeparableFilter_BilateralFilter_ps.hlsl").c_str(), macro, 
            D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", 
            "ps_5_1", compileFlags, 0, &vPassPS[i]));
        macro[1].Definition = "1";
        V(Graphics::CompileShaderFromFile(Core::GetAssetFullPath(
            L"SeparableFilter_BilateralFilter_ps.hlsl").c_str(), macro,
            D3D_COMPILE_STANDARD_FILE_INCLUDE, "main",
            "ps_5_1", compileFlags, 0, &hPassPS[i]));

        _hPassPSO[i].SetRootSignature(_rootSignature);
        _hPassPSO[i].SetRasterizerState(Graphics::g_RasterizerDefault);
        _hPassPSO[i].SetBlendState(Graphics::g_BlendDisable);
        _hPassPSO[i].SetDepthStencilState(Graphics::g_DepthStateDisabled);
        _hPassPSO[i].SetSampleMask(0xFFFFFFFF);
        _hPassPSO[i].SetInputLayout(0, nullptr);
        _hPassPSO[i].SetVertexShader(
            quadVS->GetBufferPointer(), quadVS->GetBufferSize());
        _hPassPSO[i].SetRenderTargetFormats(
            1, &bufferFormat, DXGI_FORMAT_UNKNOWN);
        _hPassPSO[i].SetPrimitiveTopologyType(
            D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
        _vPassPSO[i] = _hPassPSO[i];
        _vPassPSO[i].SetPixelShader(
            vPassPS[i]->GetBufferPointer(), vPassPS[i]->GetBufferSize());
        _vPassPSO[i].Finalize();
        _hPassPSO[i].SetPixelShader(
            hPassPS[i]->GetBufferPointer(), hPassPS[i]->GetBufferSize());
        _hPassPSO[i].Finalize();
    }
    return hr;
}

void SeperableFilter::UpdateCB(DirectX::XMUINT2 reso)
{
    if (_dataCB.u2Reso.x != reso.x || _dataCB.u2Reso.y != reso.y) {
        if (_intermediateBuf.GetResource() != nullptr) {
            _intermediateBuf.Destroy();
        }
        _intermediateBuf.Create(L"BilateralTemp", 
            (uint32_t)reso.x, (uint32_t)reso.y, 1, _outTexFormat);
        _dataCB.u2Reso = reso;
        _viewport.Width = static_cast<float>(reso.x);
        _viewport.Height = static_cast<float>(reso.y);
        _viewport.MaxDepth = 1.0;
        _scisorRact.right = static_cast<LONG>(reso.x);
        _scisorRact.bottom = static_cast<LONG>(reso.y);
    }
}

void SeperableFilter::OnRender(
    GraphicsContext& gfxCtx, const ColorBuffer* pInputTex)
{
    GPU_PROFILE(gfxCtx, L"BilateralFilter");
    gfxCtx.SetRootSignature(_rootSignature);
    gfxCtx.SetPipelineState(_hPassPSO[_kernelSizeInUse]);
    gfxCtx.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    gfxCtx.SetDynamicConstantBufferView(0, sizeof(CBuffer), (void*)&_dataCB);
    gfxCtx.SetDynamicDescriptors(1, 0, 1, &pInputTex->GetSRV());
    gfxCtx.SetRenderTargets(1, &_intermediateBuf.GetRTV());
    gfxCtx.SetViewport(_viewport);
    gfxCtx.SetScisor(_scisorRact);
    gfxCtx.TransitionResource(
        _intermediateBuf, D3D12_RESOURCE_STATE_RENDER_TARGET);
    gfxCtx.Draw(3);

    gfxCtx.TransitionResource(
        _intermediateBuf, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    gfxCtx.SetPipelineState(_vPassPSO[_kernelSizeInUse]);
    gfxCtx.SetDynamicDescriptors(1, 0, 1, &_intermediateBuf.GetSRV());
    gfxCtx.SetRenderTargets(1, &pInputTex->GetRTV());
    gfxCtx.Draw(3);
}

void SeperableFilter::RenderGui()
{
    if (ImGui::CollapsingHeader("BilateralFilter Settings")) {
        ImGui::SliderInt("Kernel Radius", 
            (int*)&_kernelSizeInUse, 0, kNumKernelDiameter - 1);
        ImGui::DragFloat("Range Variance", 
            &_dataCB.fGaussianVar, 1.f, 100, 2500);
    }
}