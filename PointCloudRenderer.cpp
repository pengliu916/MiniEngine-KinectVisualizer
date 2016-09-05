#include "pch.h"
#include "PointCloudRenderer_SharedHeader.inl"
#include "PointCloudRenderer.h"

using namespace DirectX;
using namespace Microsoft::WRL;

PointCloudRenderer::PointCloudRenderer()
{
    m_CBData.Offset = XMFLOAT4(0.f, 0.f, -2.5f, 0.f);
    m_CBData.AmbientCol = XMFLOAT4(0.1f, 0.1f, 0.1f, 0.0f);
    m_CBData.LightAttn = XMFLOAT4(1.f, 0.01f, 0.0f, 0.0f);
    m_CBData.LightPos = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
    m_CBData.MaxQuadDepDiff = 0.2f;
}

PointCloudRenderer::~PointCloudRenderer()
{
}

HRESULT PointCloudRenderer::OnCreateResource()
{
    HRESULT hr = S_OK;

    // Create Rootsignature
    m_RootSignature.Reset(2);
    m_RootSignature[0].InitAsConstantBuffer(0);
    m_RootSignature[1].InitAsDescriptorRange(
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 2);
    m_RootSignature.Finalize(L"PointCloudRenderer",
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS);

    // Create PSO
    ComPtr<ID3DBlob> HightMapVS;
    ComPtr<ID3DBlob> PassThroughVS;
    ComPtr<ID3DBlob> TexPS;
    ComPtr<ID3DBlob> TexNormalPS;
    ComPtr<ID3DBlob> NormalSurfaceGS;
    ComPtr<ID3DBlob> SurfaceGS;

    uint32_t compileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    D3D_SHADER_MACRO macro[] = {
        {"__hlsl", "1"}, // 0
        {"HeightMapVS", "1"}, // 1
        {"PassThroughVS", "0"}, // 2
        {"TexPS", "0"}, // 3
        {"NormalSurfaceGS", "0"}, // 4
        {"SurfaceGS", "0"}, // 5
        {"TexNormalPS", "0"}, // 6
        {nullptr, nullptr}
    };
    V(Graphics::CompileShaderFromFile(
        Core::GetAssetFullPath(L"PointCloudRenderer.hlsl").c_str(), macro, 
        D3D_COMPILE_STANDARD_FILE_INCLUDE, "vs_heightmap_main", "vs_5_1", 
        compileFlags, 0, &HightMapVS));
    macro[1].Definition = "0";
    macro[2].Definition = "1";
    V(Graphics::CompileShaderFromFile(
        Core::GetAssetFullPath(L"PointCloudRenderer.hlsl").c_str(), macro, 
        D3D_COMPILE_STANDARD_FILE_INCLUDE, "vs_passthrough_main", "vs_5_1", 
        compileFlags, 0, &PassThroughVS));
    macro[2].Definition = "0";
    macro[3].Definition = "1";
    V(Graphics::CompileShaderFromFile(
        Core::GetAssetFullPath(L"PointCloudRenderer.hlsl").c_str(), macro, 
        D3D_COMPILE_STANDARD_FILE_INCLUDE, "ps_tex_main", "ps_5_1", 
        compileFlags, 0, &TexPS));
    macro[3].Definition = "0";
    macro[4].Definition = "1";
    V(Graphics::CompileShaderFromFile(
        Core::GetAssetFullPath(L"PointCloudRenderer.hlsl").c_str(), macro, 
        D3D_COMPILE_STANDARD_FILE_INCLUDE, "gs_normalsurface_main", "gs_5_1", 
        compileFlags, 0, &NormalSurfaceGS));
    macro[4].Definition = "0";
    macro[5].Definition = "1";
    V(Graphics::CompileShaderFromFile(
        Core::GetAssetFullPath(L"PointCloudRenderer.hlsl").c_str(), macro, 
        D3D_COMPILE_STANDARD_FILE_INCLUDE, "gs_surface_main", "gs_5_1", 
        compileFlags, 0, &SurfaceGS));
    macro[5].Definition = "0";
    macro[6].Definition = "1";
    V(Graphics::CompileShaderFromFile(
        Core::GetAssetFullPath(L"PointCloudRenderer.hlsl").c_str(), macro, 
        D3D_COMPILE_STANDARD_FILE_INCLUDE, "ps_texnormal_main", "ps_5_1", 
        compileFlags, 0, &TexNormalPS));

    m_RenderModePSO[kPointCloud].SetRootSignature(m_RootSignature);
    m_RenderModePSO[kPointCloud].SetRasterizerState(
        Graphics::g_RasterizerTwoSided);
    m_RenderModePSO[kPointCloud].SetBlendState(Graphics::g_BlendDisable);
    m_RenderModePSO[kPointCloud].SetDepthStencilState(
        Graphics::g_DepthStateReadWrite);
    m_RenderModePSO[kPointCloud].SetSampleMask(0xFFFFFFFF);
    m_RenderModePSO[kPointCloud].SetInputLayout(0, nullptr);
    m_RenderModePSO[kPointCloud].SetPixelShader(
        TexPS->GetBufferPointer(), TexPS->GetBufferSize());
    m_RenderModePSO[kPointCloud].SetRenderTargetFormats(
        1, &Graphics::g_SceneColorBuffer.GetFormat(), 
        Graphics::g_SceneDepthBuffer.GetFormat());
    m_RenderModePSO[kSurface] = m_RenderModePSO[kPointCloud];
    m_RenderModePSO[kSurface].SetPrimitiveTopologyType(
        D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT);
    m_RenderModePSO[kSurface].SetVertexShader(
        PassThroughVS->GetBufferPointer(), PassThroughVS->GetBufferSize());
    m_RenderModePSO[kShadedSurface] = m_RenderModePSO[kSurface];
    m_RenderModePSO[kShadedSurface].SetGeometryShader(
        NormalSurfaceGS->GetBufferPointer(), NormalSurfaceGS->GetBufferSize());
    m_RenderModePSO[kShadedSurface].SetPixelShader(
        TexNormalPS->GetBufferPointer(), TexNormalPS->GetBufferSize());
    m_RenderModePSO[kShadedSurface].Finalize();
    m_RenderModePSO[kSurface].SetGeometryShader(
        SurfaceGS->GetBufferPointer(), SurfaceGS->GetBufferSize());
    m_RenderModePSO[kSurface].Finalize();
    m_RenderModePSO[kPointCloud].SetVertexShader(
        HightMapVS->GetBufferPointer(), HightMapVS->GetBufferSize());
    m_RenderModePSO[kPointCloud].SetPrimitiveTopologyType(
        D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT);
    m_RenderModePSO[kPointCloud].Finalize();

    return hr;
}

void PointCloudRenderer::UpdateCB(DirectX::XMFLOAT2 ColReso, 
    DirectX::XMFLOAT2 DepReso, DirectX::XMFLOAT4 ColFC, 
    DirectX::XMFLOAT4 DepFC, DirectX::XMMATRIX Dep2Col)
{
    m_CBData.ColorReso = ColReso;
    m_CBData.DepthInfraredReso = DepReso;
    m_CBData.ColorCxyFxy = ColFC;
    m_CBData.DepthCxyFxy = DepFC;
    m_CBData.Depth2Color = Dep2Col;
}

void PointCloudRenderer::OnRender(GraphicsContext& gfxContext, 
    ColorBuffer* pDepthMap, ColorBuffer* pColorMap, XMMATRIX viewProj)
{
    GPU_PROFILE(gfxContext, L"HeightMap Render");

    gfxContext.SetRootSignature(m_RootSignature);
    gfxContext.SetPipelineState(m_RenderModePSO[m_RenderMode]);
    gfxContext.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);

    m_CBData.ViewProjMat = viewProj;
    gfxContext.ClearColor(Graphics::g_SceneColorBuffer);
    gfxContext.ClearDepth(Graphics::g_SceneDepthBuffer);
    gfxContext.SetDynamicConstantBufferView(
        0, sizeof(CBuffer), (void*)&m_CBData);
    gfxContext.SetDynamicDescriptors(1, 0, 1, &pDepthMap->GetSRV());
    gfxContext.SetDynamicDescriptors(1, 1, 1, &pColorMap->GetSRV());
    gfxContext.SetRenderTargets(
        1, &Graphics::g_SceneColorBuffer.GetRTV(), 
        Graphics::g_SceneDepthBuffer.GetDSV());
    gfxContext.SetViewport(Graphics::g_DisplayPlaneViewPort);
    gfxContext.SetScisor(Graphics::g_DisplayPlaneScissorRect);
    gfxContext.Draw(pDepthMap->GetWidth() * pDepthMap->GetHeight());
}

void PointCloudRenderer::RenderGui()
{
    if (ImGui::CollapsingHeader("PointCloud Settings"))
    {
        ImGui::RadioButton(
            "Points", (int*)&m_RenderMode, RenderMode::kPointCloud);
        ImGui::SameLine();
        ImGui::RadioButton(
            "Surface", (int*)&m_RenderMode, RenderMode::kSurface);
        ImGui::SameLine();
        ImGui::RadioButton(
            "Shaded", (int*)&m_RenderMode, RenderMode::kShadedSurface);
        static float offset[3] = 
            {m_CBData.Offset.x, m_CBData.Offset.y, m_CBData.Offset.z};
        ImGui::DragFloat3("PC Offset", offset, 0.1f);
        if (m_RenderMode != PointCloudRenderer::kPointCloud) {
            ImGui::SliderFloat(
                "MaxQuadDepthDiff", &m_CBData.MaxQuadDepDiff, 0.01f, 0.15f);
        }
        m_CBData.Offset.x = offset[0];
        m_CBData.Offset.y = offset[1];
        m_CBData.Offset.z = offset[2];
    }
}