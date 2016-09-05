#include "pch.h"
#include "SeparableFilter.h"

using namespace DirectX;
using namespace Microsoft::WRL;

SeperableFilter::SeperableFilter()
{
    m_CBData.Reso = XMUINT2(0, 0);
    // We set 50mm as edge threshold, so 50 will be 2*deviation
    // ->deviation = 25;
    // ->variance = 625
    m_CBData.DiffrenceVariance = 625;
}

SeperableFilter::~SeperableFilter()
{
}

HRESULT SeperableFilter::OnCreateResoure(DXGI_FORMAT BufferFormat)
{
    HRESULT hr = S_OK;

    // Create Rootsignature
    m_RootSignature.Reset(2);
    m_RootSignature[0].InitAsConstantBuffer(0);
    m_RootSignature[1].InitAsDescriptorRange(
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 1);
    m_RootSignature.Finalize(L"SeparableFilter",
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS);

    // Create PSO
    ComPtr<ID3DBlob> QuadVS;
    ComPtr<ID3DBlob> HorizontalPS[kNumKernelDiameter];
    ComPtr<ID3DBlob> VerticalPS[kNumKernelDiameter];

    uint32_t compileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    D3D_SHADER_MACRO macro[] = {
        {"__hlsl", "1"}, // 0
        {"QuadVS", "1"}, // 1
        {"FilterPS", "0"}, // 2
        {"HorizontalPass", "0"}, // 3
        {"KERNEL_RADIUS", "0"}, // 4
        {nullptr, nullptr}
    };
    V(Graphics::CompileShaderFromFile(
        Core::GetAssetFullPath(L"BilateralFilter.hlsl").c_str(), macro, 
        D3D_COMPILE_STANDARD_FILE_INCLUDE, "vs_quad_main", "vs_5_1", 
        compileFlags, 0, &QuadVS));
    macro[1].Definition = "0";
    macro[2].Definition = "1";
    char kernelSizeStr[8];
    for (int i = 0; i < kNumKernelDiameter; ++i) {
        sprintf(kernelSizeStr, "%d", i);
        macro[4].Definition = kernelSizeStr;
        macro[3].Definition = "0";
        V(Graphics::CompileShaderFromFile(
            Core::GetAssetFullPath(L"BilateralFilter.hlsl").c_str(), macro, 
            D3D_COMPILE_STANDARD_FILE_INCLUDE, "ps_bilateralfilter_main", 
            "ps_5_1", compileFlags, 0, &VerticalPS[i]));
        macro[3].Definition = "1";
        V(Graphics::CompileShaderFromFile(
            Core::GetAssetFullPath(L"BilateralFilter.hlsl").c_str(), macro, 
            D3D_COMPILE_STANDARD_FILE_INCLUDE, "ps_bilateralfilter_main", 
            "ps_5_1", compileFlags, 0, &HorizontalPS[i]));

        m_HPassPSO[i].SetRootSignature(m_RootSignature);
        m_HPassPSO[i].SetRasterizerState(Graphics::g_RasterizerDefault);
        m_HPassPSO[i].SetBlendState(Graphics::g_BlendDisable);
        m_HPassPSO[i].SetDepthStencilState(Graphics::g_DepthStateDisabled);
        m_HPassPSO[i].SetSampleMask(0xFFFFFFFF);
        m_HPassPSO[i].SetInputLayout(0, nullptr);
        m_HPassPSO[i].SetVertexShader(
            QuadVS->GetBufferPointer(), QuadVS->GetBufferSize());
        m_HPassPSO[i].SetRenderTargetFormats(
            1, &BufferFormat, DXGI_FORMAT_UNKNOWN);
        m_HPassPSO[i].SetPrimitiveTopologyType(
            D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
        m_VPassPSO[i] = m_HPassPSO[i];
        m_VPassPSO[i].SetPixelShader(
            VerticalPS[i]->GetBufferPointer(), 
            VerticalPS[i]->GetBufferSize());
        m_VPassPSO[i].Finalize();
        m_HPassPSO[i].SetPixelShader(
            HorizontalPS[i]->GetBufferPointer(), 
            HorizontalPS[i]->GetBufferSize());
        m_HPassPSO[i].Finalize();
    }
    return hr;
}

void SeperableFilter::UpdateCB(DirectX::XMUINT2 Reso, DXGI_FORMAT Format)
{
    if (m_CBData.Reso.x != Reso.x || m_CBData.Reso.y != Reso.y) {
        if (m_WorkingBuffer.GetResource() != nullptr) {
            m_WorkingBuffer.Destroy();
        }
        m_WorkingBuffer.Create(L"BilateralTemp", 
            (uint32_t)Reso.x, (uint32_t)Reso.y, 1, Format);
        m_CBData.Reso = Reso;
        m_Viewport.Width = static_cast<float>(Reso.x);
        m_Viewport.Height = static_cast<float>(Reso.y);
        m_Viewport.MaxDepth = 1.0;
        m_ScisorRact.right = static_cast<LONG>(Reso.x);
        m_ScisorRact.bottom = static_cast<LONG>(Reso.y);
    }
}

void SeperableFilter::OnRender(
    GraphicsContext& gfxContext, ColorBuffer* pInputTex)
{
    GPU_PROFILE(gfxContext, L"BilateralFilter");
    gfxContext.SetRootSignature(m_RootSignature);
    gfxContext.SetPipelineState(m_HPassPSO[m_KernelSizeInUse]);
    gfxContext.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    gfxContext.SetDynamicConstantBufferView(
        0, sizeof(CBuffer), (void*)&m_CBData);
    gfxContext.SetDynamicDescriptors(1, 0, 1, &pInputTex->GetSRV());
    gfxContext.SetRenderTargets(1, &m_WorkingBuffer.GetRTV());
    gfxContext.SetViewport(m_Viewport);
    gfxContext.SetScisor(m_ScisorRact);
    gfxContext.TransitionResource(
        m_WorkingBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET);
    gfxContext.Draw(3);

    gfxContext.TransitionResource(
        m_WorkingBuffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    gfxContext.SetPipelineState(m_VPassPSO[m_KernelSizeInUse]);
    gfxContext.SetDynamicDescriptors(1, 0, 1, &m_WorkingBuffer.GetSRV());
    gfxContext.SetRenderTargets(1, &pInputTex->GetRTV());
    gfxContext.Draw(3);
}

void SeperableFilter::RenderGui()
{
    if (ImGui::CollapsingHeader("BilateralFilter Settings")) {
        ImGui::SliderInt("Kernel Radius", 
            (int*)&m_KernelSizeInUse, 0, kNumKernelDiameter - 1);
        ImGui::DragFloat("Range Variance", 
            &m_CBData.DiffrenceVariance, 1.f, 100, 2500);
    }
}