#include "pch.h"
#include "PointCloudRenderer.inl"
#include "PointCloudRenderer.h"

using namespace DirectX;
using namespace Microsoft::WRL;

PointCloudRenderer::PointCloudRenderer()
{
    _cbData.f4Offset = XMFLOAT4(0.f, 0.f, -2.5f, 0.f);
    _cbData.f4AmbientCol = XMFLOAT4(0.1f, 0.1f, 0.1f, 0.0f);
    _cbData.f4LightAttn = XMFLOAT4(1.f, 0.01f, 0.0f, 0.0f);
    _cbData.f4LightPos = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
    _cbData.fQuadZSpanThreshold = 0.2f;
}

PointCloudRenderer::~PointCloudRenderer()
{
}

HRESULT
PointCloudRenderer::OnCreateResource()
{
    HRESULT hr = S_OK;

    // Create Rootsignature
    _rootSignature.Reset(2);
    _rootSignature[0].InitAsConstantBuffer(0);
    _rootSignature[1].InitAsDescriptorRange(
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 2);
    _rootSignature.Finalize(L"PointCloudRenderer",
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS);

    // Create PSO
    ComPtr<ID3DBlob> pointCloudVS;
    ComPtr<ID3DBlob> passThroughVS;
    ComPtr<ID3DBlob> planeTexColorPS;
    ComPtr<ID3DBlob> shadedTexColorPS;
    ComPtr<ID3DBlob> normalTriangleCloudGS;
    ComPtr<ID3DBlob> triangleCloudGS;

    uint32_t compileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    D3D_SHADER_MACRO macro[] = {
        {"__hlsl", "1"}, // 0
        {"SHADED", "0"}, // 1
        {nullptr, nullptr}
    };
    V(Graphics::CompileShaderFromFile(Core::GetAssetFullPath(
        L"PointCloudRenderer_PointCloud_vs.hlsl").c_str(), macro, 
        D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "vs_5_1", 
        compileFlags, 0, &pointCloudVS));
    V(Graphics::CompileShaderFromFile(Core::GetAssetFullPath(
        L"PointCloudRenderer_PassThrough_vs.hlsl").c_str(), macro, 
        D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "vs_5_1", 
        compileFlags, 0, &passThroughVS));
    V(Graphics::CompileShaderFromFile(Core::GetAssetFullPath(
        L"PointCloudRenderer_TexColor_ps.hlsl").c_str(), macro, 
        D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "ps_5_1", 
        compileFlags, 0, &planeTexColorPS));
    V(Graphics::CompileShaderFromFile(Core::GetAssetFullPath(
        L"PointCloudRenderer_TriangleCloud_gs.hlsl").c_str(), macro, 
        D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "gs_5_1", 
        compileFlags, 0, &triangleCloudGS));
    macro[1].Definition = "1"; // turn on SHADED flag
    V(Graphics::CompileShaderFromFile(Core::GetAssetFullPath(
        L"PointCloudRenderer_TriangleCloud_gs.hlsl").c_str(), macro, 
        D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "gs_5_1", 
        compileFlags, 0, &normalTriangleCloudGS));
    V(Graphics::CompileShaderFromFile(Core::GetAssetFullPath(
        L"PointCloudRenderer_TexColor_ps.hlsl").c_str(), macro, 
        D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "ps_5_1", 
        compileFlags, 0, &shadedTexColorPS));

    _gfxRenderPSO[kPointCloud].SetRootSignature(_rootSignature);
    _gfxRenderPSO[kPointCloud].SetRasterizerState(
        Graphics::g_RasterizerTwoSided);
    _gfxRenderPSO[kPointCloud].SetBlendState(Graphics::g_BlendDisable);
    _gfxRenderPSO[kPointCloud].SetDepthStencilState(
        Graphics::g_DepthStateReadWrite);
    _gfxRenderPSO[kPointCloud].SetSampleMask(0xFFFFFFFF);
    _gfxRenderPSO[kPointCloud].SetInputLayout(0, nullptr);
    _gfxRenderPSO[kPointCloud].SetPixelShader(
        planeTexColorPS->GetBufferPointer(), planeTexColorPS->GetBufferSize());
    _gfxRenderPSO[kPointCloud].SetRenderTargetFormats(
        1, &Graphics::g_SceneColorBuffer.GetFormat(), 
        Graphics::g_SceneDepthBuffer.GetFormat());
    _gfxRenderPSO[kSurface] = _gfxRenderPSO[kPointCloud];
    _gfxRenderPSO[kSurface].SetPrimitiveTopologyType(
        D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT);
    _gfxRenderPSO[kSurface].SetVertexShader(
        passThroughVS->GetBufferPointer(), passThroughVS->GetBufferSize());
    _gfxRenderPSO[kShadedSurface] = _gfxRenderPSO[kSurface];
    _gfxRenderPSO[kShadedSurface].SetGeometryShader(
        normalTriangleCloudGS->GetBufferPointer(),
        normalTriangleCloudGS->GetBufferSize());
    _gfxRenderPSO[kShadedSurface].SetPixelShader(
        shadedTexColorPS->GetBufferPointer(),
        shadedTexColorPS->GetBufferSize());
    _gfxRenderPSO[kShadedSurface].Finalize();
    _gfxRenderPSO[kSurface].SetGeometryShader(
        triangleCloudGS->GetBufferPointer(), triangleCloudGS->GetBufferSize());
    _gfxRenderPSO[kSurface].Finalize();
    _gfxRenderPSO[kPointCloud].SetVertexShader(
        pointCloudVS->GetBufferPointer(), pointCloudVS->GetBufferSize());
    _gfxRenderPSO[kPointCloud].SetPrimitiveTopologyType(
        D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT);
    _gfxRenderPSO[kPointCloud].Finalize();

    return hr;
}

void
PointCloudRenderer::UpdateLightPos(const DirectX::XMFLOAT4& pos)
{
    _cbData.f4LightPos = pos;
}

void
PointCloudRenderer::UpdateCB(const DirectX::XMFLOAT2& colReso, 
    const DirectX::XMFLOAT2& depReso, const DirectX::XMFLOAT4& colFC, 
    const DirectX::XMFLOAT4& depFC, const DirectX::XMMATRIX& dep2Col)
{
    _cbData.f2ColorReso = colReso;
    _cbData.f2DepthInfraredReso = depReso;
    _cbData.f4ColorCxyFxy = colFC;
    _cbData.f4DepthCxyFxy = depFC;
    _cbData.mDepth2Color = dep2Col;
}

void
PointCloudRenderer::OnRender(GraphicsContext& gfxContext, 
    const ColorBuffer* pDepthMap, const ColorBuffer* pColorMap,
    const DirectX::XMMATRIX& viewProj)
{
    GPU_PROFILE(gfxContext, L"HeightMap Render");

    gfxContext.SetRootSignature(_rootSignature);
    gfxContext.SetPipelineState(_gfxRenderPSO[_renderMode]);
    gfxContext.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);

    gfxContext.TransitionResource(Graphics::g_SceneColorBuffer,
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    gfxContext.TransitionResource(Graphics::g_SceneDepthBuffer,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, true);
    _cbData.mViewProj = viewProj;
    gfxContext.ClearColor(Graphics::g_SceneColorBuffer);
    gfxContext.ClearDepth(Graphics::g_SceneDepthBuffer);
    gfxContext.SetDynamicConstantBufferView(
        0, sizeof(CBuffer), (void*)&_cbData);
    gfxContext.SetDynamicDescriptors(1, 0, 1, &pDepthMap->GetSRV());
    gfxContext.SetDynamicDescriptors(1, 1, 1, &pColorMap->GetSRV());
    gfxContext.SetRenderTargets(
        1, &Graphics::g_SceneColorBuffer.GetRTV(), 
        Graphics::g_SceneDepthBuffer.GetDSV());
    gfxContext.SetViewport(Graphics::g_DisplayPlaneViewPort);
    gfxContext.SetScisor(Graphics::g_DisplayPlaneScissorRect);
    gfxContext.Draw(pDepthMap->GetWidth() * pDepthMap->GetHeight());
}

void
PointCloudRenderer::RenderGui()
{
    if (ImGui::CollapsingHeader("PointCloud Settings"))
    {
        ImGui::RadioButton(
            "Points", (int*)&_renderMode, RenderMode::kPointCloud);
        ImGui::SameLine();
        ImGui::RadioButton(
            "Surface", (int*)&_renderMode, RenderMode::kSurface);
        ImGui::SameLine();
        ImGui::RadioButton(
            "Shaded", (int*)&_renderMode, RenderMode::kShadedSurface);
        static float offset[3] = 
            {_cbData.f4Offset.x, _cbData.f4Offset.y, _cbData.f4Offset.z};
        ImGui::DragFloat3("PC Offset", offset, 0.1f);
        if (_renderMode != PointCloudRenderer::kPointCloud) {
            ImGui::SliderFloat(
                "MaxQuadZSpan", &_cbData.fQuadZSpanThreshold, 0.01f, 0.15f);
        }
        _cbData.f4Offset.x = offset[0];
        _cbData.f4Offset.y = offset[1];
        _cbData.f4Offset.z = offset[2];
    }
}