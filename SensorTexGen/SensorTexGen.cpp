#include "pch.h"
#include "SensorTexGen.h"

using namespace DirectX;
using namespace Microsoft::WRL;

namespace {
    bool _useCS = true;
    bool _streaming = true;
    bool _perFrameUpdate = true;
    std::string _texNames[SensorTexGen::kNumTargetTex] =
    {"Depth Raw","Depth Visualized","Infrared Gamma","Color Raw"};

    RootSignature _rootSignature;
    GraphicsPSO
        _gfxDepthPSO[SensorTexGen::kNumDepthMode][SensorTexGen::kNumDataMode];
    GraphicsPSO
        _gfxColorPSO[SensorTexGen::kNumColorMode][SensorTexGen::kNumDataMode];
    ComputePSO
        _cptDepthPSO[SensorTexGen::kNumDepthMode][SensorTexGen::kNumDataMode];
    ComputePSO
        _cptColorPSO[SensorTexGen::kNumColorMode][SensorTexGen::kNumDataMode];
}

SensorTexGen::SensorTexGen(
    bool enableColor, bool enableDepth, bool enableInfrared)
{
    _preColorMode = _colorMode;
    _preDepthMode = _depthMode;
    _pKinect2 = StreamFactory::createFromKinect2(
        enableColor, enableDepth, enableInfrared);
    _pKinect2->StartStream();
}

SensorTexGen::~SensorTexGen()
{
}

void
SensorTexGen::OnCreateResource()
{
    HRESULT hr = S_OK;
    uint16_t depWidth, depHeight;
    _pKinect2->GetDepthReso(depWidth, depHeight);

    _cbKinect.f2DepthInfraredReso = XMFLOAT2(depWidth, depHeight);

    _pFrameAlloc[IRGBDStreamer::kDepth] = new LinearFrameAllocator(
        depWidth * depHeight, sizeof(uint16_t), DXGI_FORMAT_R16_UINT);
    _outTex[kDepthVisualTex].Create(L"Depth Visual Tex",
        depWidth, depHeight, 1, DXGI_FORMAT_R11G11B10_FLOAT);
    _outTex[kDepthTex].Create(L"Depth Raw Tex",
        depWidth, depHeight, 1, DXGI_FORMAT_R16_UINT);

    _pFrameAlloc[IRGBDStreamer::kInfrared] = new LinearFrameAllocator(
        depWidth * depHeight, sizeof(uint16_t), DXGI_FORMAT_R16_UINT);
    _outTex[kInfraredTex].Create(L"Infrared Tex",
        depWidth, depHeight, 1, DXGI_FORMAT_R11G11B10_FLOAT);

    _depthInfraredViewport = {};
    _depthInfraredViewport.Width = depWidth;
    _depthInfraredViewport.Height = depHeight;
    _depthInfraredViewport.MaxDepth = 1.0f;
    _depthInfraredScissorRect = {};
    _depthInfraredScissorRect.right = depWidth;
    _depthInfraredScissorRect.bottom = depHeight;

    uint16_t colWidth, colHeight;
    _pKinect2->GetColorReso(colWidth, colHeight);

    _cbKinect.f2ColorReso = XMFLOAT2(colWidth, colHeight);

    _pFrameAlloc[IRGBDStreamer::kColor] = new LinearFrameAllocator(
        colWidth * colHeight, sizeof(uint32_t), DXGI_FORMAT_R8G8B8A8_UINT);
    _outTex[kColorTex].Create(L"Color Tex",
        colWidth, colHeight, 1, DXGI_FORMAT_R11G11B10_FLOAT);

    for (uint16_t i = 0; i < kNumTargetTex; ++i) {
        _outTextureRTV[i] = _outTex[i].GetRTV();
    }

    _colorViewport = {};
    _colorViewport.Width = colWidth;
    _colorViewport.Height = colHeight;
    _colorViewport.MaxDepth = 1.0f;
    _colorScissorRect = {};
    _colorScissorRect.right = colWidth;
    _colorScissorRect.bottom = colHeight;

    // Create rootsignature
    _rootSignature.Reset(3);
    _rootSignature[0].InitAsConstantBuffer(0);
    _rootSignature[1].InitAsDescriptorRange(
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, IRGBDStreamer::kNumBufferTypes);
    _rootSignature[2].InitAsDescriptorRange(
        D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0, IRGBDStreamer::kNumBufferTypes + 1);
    _rootSignature.Finalize(L"SensorTexGen",
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS);

    // Create PSO
    ComPtr<ID3DBlob> quadVS;
    ComPtr<ID3DBlob> copyDepthPS[kNumDepthMode][kNumDataMode];
    ComPtr<ID3DBlob> copyColorPS[kNumColorMode][kNumDataMode];
    ComPtr<ID3DBlob> copyDepthCS[kNumDepthMode][kNumDataMode];
    ComPtr<ID3DBlob> copyColorCS[kNumColorMode][kNumDataMode];
    uint32_t compileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    D3D_SHADER_MACRO macro[] = {
        {"__hlsl", "1"}, // 0
        {"UNDISTORTION", "0"}, // 1
        {"VISUALIZED_DEPTH_TEX", "0"}, // 2
        {"INFRARED_TEX", "0"}, // 3
        {"COLOR_TEX", "0"}, // 4
        {nullptr, nullptr}
    };
    V(Graphics::CompileShaderFromFile(Core::GetAssetFullPath(
        _T("SensorTexGen_SingleTriangleQuad_vs.hlsl")).c_str(), macro,
        D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "vs_5_1",
        compileFlags, 0, &quadVS));

    char tempChar[8];
    for (int i = 0; i < kNumDataMode; ++i) {
        sprintf_s(tempChar, 8, "%d", i);
        macro[1].Definition = tempChar; // UNDISTORTION
        for (int j = 0; j < kNumDepthMode; ++j) {
            switch (j) {
            case kDepthWithVisualWithInfrared:
                macro[3].Definition = "1"; // INFRARED_TEX
            case kDepthVisualTex:
                macro[2].Definition = "1"; // VISUALIZED_DEPTH_TEX
            }
            V(Graphics::CompileShaderFromFile(Core::GetAssetFullPath(
                _T("SensorTexGen_Copy_ps.hlsl")).c_str(), macro,
                D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "ps_5_1",
                compileFlags, 0, &copyDepthPS[j][i]));
            V(Graphics::CompileShaderFromFile(Core::GetAssetFullPath(
                _T("SensorTexGen_Copy_cs.hlsl")).c_str(), macro,
                D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "cs_5_1",
                compileFlags, 0, &copyDepthCS[j][i]));

            macro[2].Definition = "0"; // VISUALIZED_DEPTH_TEX
            macro[3].Definition = "0"; // INFRARED_TEX
        }
        macro[4].Definition = "1"; // COLOR_TEX
        V(Graphics::CompileShaderFromFile(Core::GetAssetFullPath(
            _T("SensorTexGen_Copy_ps.hlsl")).c_str(), macro,
            D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "ps_5_1",
            compileFlags, 0, &copyColorPS[0][i]));
        V(Graphics::CompileShaderFromFile(Core::GetAssetFullPath(
            _T("SensorTexGen_Copy_cs.hlsl")).c_str(), macro,
            D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "cs_5_1",
            compileFlags, 0, &copyColorCS[0][i]));
        macro[4].Definition = "0"; // COLOR_TEX
    }

    DXGI_FORMAT RTformat[] = {
        _outTex[kDepthTex].GetFormat(),
        _outTex[kDepthVisualTex].GetFormat(),
        _outTex[kInfraredTex].GetFormat(),
        _outTex[kColorTex].GetFormat()
    };

    for (int i = 0; i < kNumDataMode; ++i) {
        _gfxColorPSO[0][i].SetRootSignature(_rootSignature);
        _gfxColorPSO[0][i].SetRasterizerState(Graphics::g_RasterizerTwoSided);
        _gfxColorPSO[0][i].SetBlendState(Graphics::g_BlendDisable);
        _gfxColorPSO[0][i].SetDepthStencilState(Graphics::g_DepthStateDisabled);
        _gfxColorPSO[0][i].SetSampleMask(0xFFFFFFFF);
        _gfxColorPSO[0][i].SetInputLayout(0, nullptr);
        _gfxColorPSO[0][i].SetPrimitiveTopologyType(
            D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
        _gfxColorPSO[0][i].SetVertexShader(
            quadVS->GetBufferPointer(), quadVS->GetBufferSize());
        _gfxColorPSO[0][i].SetRenderTargetFormats(
            1, &_outTex[kColorTex].GetFormat(), DXGI_FORMAT_UNKNOWN);
        _gfxColorPSO[0][i].SetPixelShader(copyColorPS[0][i]->GetBufferPointer(),
            copyColorPS[0][i]->GetBufferSize());
        _cptColorPSO[0][i].SetRootSignature(_rootSignature);
        _cptColorPSO[0][i].SetComputeShader(
            copyColorCS[0][i]->GetBufferPointer(),
            copyColorCS[0][i]->GetBufferSize());
        _cptColorPSO[0][i].Finalize();

        for (int j = 0; j < kNumDepthMode; ++j) {
            _gfxDepthPSO[j][i] = _gfxColorPSO[0][i];
            _gfxDepthPSO[j][i].SetPixelShader(
                copyDepthPS[j][i]->GetBufferPointer(),
                copyDepthPS[j][i]->GetBufferSize());
            _gfxDepthPSO[j][i].SetRenderTargetFormats(
                j + 1, RTformat, DXGI_FORMAT_UNKNOWN);
            _gfxDepthPSO[j][i].Finalize();

            _cptDepthPSO[j][i].SetRootSignature(_rootSignature);
            _cptDepthPSO[j][i].SetComputeShader(
                copyDepthCS[j][i]->GetBufferPointer(),
                copyDepthCS[j][i]->GetBufferSize());
            _cptDepthPSO[j][i].Finalize();
        }
        _gfxColorPSO[0][i].Finalize();
    }
}

void
SensorTexGen::OnDestory()
{
    for (int i = 0; i < IRGBDStreamer::kNumBufferTypes; ++i) {
        if (_pFrameAlloc[i]) {
            _pFrameAlloc[i]->Destory();
            delete _pFrameAlloc[i];
            _pFrameAlloc[i] = nullptr;
        }
    }
    for (int i = 0; i < kNumTargetTex; ++i) {
        _outTex[i].Destroy();
    }
    if (_pKinect2) {
        delete _pKinect2;
        _pKinect2 = nullptr;
    }
}

bool
SensorTexGen::OnRender(CommandContext& EngineContext)
{
    if (!_streaming) {
        return false;
    }
    // retire Kinect buffers used for previous frame
    _RetirePreviousFrameKinectBuffer();
    // prepare and fill in new data from Kinect
    bool needUpdate = _PrepareAndFillinKinectBuffers() || _perFrameUpdate;
    // Render to screen
    if (_useCS && needUpdate) {
        ComputeContext& ctx = EngineContext.GetComputeContext();
        GPU_PROFILE(ctx, L"Read&Present RGBD");

        ctx.SetRootSignature(_rootSignature);
        ctx.SetDynamicConstantBufferView(0, sizeof(RenderCB), &_cbKinect);

        if (_depthMode != kNoDepth) {
            ctx.SetDynamicDescriptors(1, 0, 1,
                &_pKinectBuf[IRGBDStreamer::kDepth]->GetSRV());
            ctx.SetDynamicDescriptors(2, 0, 1, &_outTex[kDepthTex].GetUAV());
            ctx.TransitionResource(_outTex[kDepthTex],
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (_depthMode != kDepth) {
                ctx.SetDynamicDescriptors(2, 1, 1,
                    &_outTex[kDepthVisualTex].GetUAV());
                ctx.TransitionResource(_outTex[kDepthVisualTex],
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
        }
        if (_depthMode == kDepthWithVisualWithInfrared) {
            ctx.SetDynamicDescriptors(1, 1, 1,
                &_pKinectBuf[IRGBDStreamer::kInfrared]->GetSRV());
            ctx.SetDynamicDescriptors(2, 2, 1, &_outTex[kInfraredTex].GetUAV());
            ctx.TransitionResource(_outTex[kInfraredTex],
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        if (_colorMode == kColor) {
            ctx.SetDynamicDescriptors(1, 2, 1,
                &_pKinectBuf[IRGBDStreamer::kColor]->GetSRV());
            ctx.SetDynamicDescriptors(2, 3, 1, &_outTex[kColorTex].GetUAV());
            ctx.TransitionResource(_outTex[kColorTex],
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        ctx.SetPipelineState(_cptDepthPSO[_depthMode][_processMode]);
        ctx.Dispatch1D(_outTex[kDepthTex].GetWidth() *
            _outTex[kDepthTex].GetHeight(), THREAD_PER_GROUP);

        if (_colorMode != kNoColor) {
            ctx.SetPipelineState(_cptColorPSO[_colorMode][_processMode]);
            ctx.Dispatch1D(_outTex[kColorTex].GetWidth() *
                _outTex[kColorTex].GetHeight(), THREAD_PER_GROUP);
        }
    } else if (needUpdate) {
        GraphicsContext& ctx = EngineContext.GetGraphicsContext();
        GPU_PROFILE(ctx, L"Read&Present RGBD");

        ctx.SetRootSignature(_rootSignature);
        ctx.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx.SetDynamicConstantBufferView(0, sizeof(RenderCB), &_cbKinect);

        if (_depthMode != kNoDepth) {
            ctx.SetDynamicDescriptors(1, 0, 1,
                &_pKinectBuf[IRGBDStreamer::kDepth]->GetSRV());
            ctx.TransitionResource(_outTex[kDepthTex],
                D3D12_RESOURCE_STATE_RENDER_TARGET);
            if (_depthMode != kDepth) {
                ctx.TransitionResource(_outTex[kDepthVisualTex],
                    D3D12_RESOURCE_STATE_RENDER_TARGET);
            }
        }
        if (_depthMode == kDepthWithVisualWithInfrared) {
            ctx.SetDynamicDescriptors(1, 1, 1,
                &_pKinectBuf[IRGBDStreamer::kInfrared]->GetSRV());
            ctx.TransitionResource(_outTex[kInfraredTex],
                D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
        if (_colorMode == kColor) {
            ctx.SetDynamicDescriptors(1, 2, 1,
                &_pKinectBuf[IRGBDStreamer::kColor]->GetSRV());
            ctx.TransitionResource(_outTex[kColorTex],
                D3D12_RESOURCE_STATE_RENDER_TARGET);
        }

        ctx.SetRenderTargets(_depthMode + 1, _outTextureRTV);
        ctx.SetViewports(1, &_depthInfraredViewport);
        ctx.SetScisors(1, &_depthInfraredScissorRect);
        ctx.SetPipelineState(_gfxDepthPSO[_depthMode][_processMode]);
        ctx.Draw(3);

        ctx.SetRenderTargets(1, &_outTextureRTV[kColorTex]);
        ctx.SetViewports(1, &_colorViewport);
        ctx.SetScisors(1, &_colorScissorRect);
        if (_colorMode != kNoColor)
        {
            ctx.SetPipelineState(_gfxColorPSO[0][_processMode]);
            ctx.Draw(3);
        }
    }
    EngineContext.TransitionResource(_outTex[kDepthTex],
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    EngineContext.TransitionResource(_outTex[kDepthVisualTex],
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    EngineContext.TransitionResource(_outTex[kInfraredTex],
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    EngineContext.TransitionResource(_outTex[kColorTex],
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return needUpdate;
}

void
SensorTexGen::RenderGui()
{
    static bool showImage[kNumTargetTex] = {};
    if (ImGui::CollapsingHeader("Sensor Tex Generator")) {
        ImGui::Checkbox("Streaming Data", &_streaming);
        ImGui::Checkbox("Update EveryFrame", &_perFrameUpdate);
        ImGui::Checkbox("Use Compute Shader", &_useCS);
        ImGui::Checkbox("Use Undistortion", (bool*)&_processMode);
        ImGui::Separator();
        ImGui::RadioButton("No Depth", (int*)&_depthMode, -1);
        ImGui::RadioButton("Depth", (int*)&_depthMode, 0);
        ImGui::RadioButton("Depth&Visual", (int*)&_depthMode, 1);
        ImGui::RadioButton("Depth&Visual&Infrared", (int*)&_depthMode, 2);
        ImGui::Separator();
        ImGui::RadioButton("No Color", (int*)&_colorMode, -1);
        ImGui::RadioButton("Color", (int*)&_colorMode, 0);

        float width = ImGui::GetContentRegionAvail().x;
        for (int i = 1; i < kNumTargetTex; ++i) {
            if (ImGui::CollapsingHeader(_texNames[i].c_str())) {
                ImTextureID tex_id = (void*)&_outTex[i].GetSRV();
                uint32_t OrigTexWidth = _outTex[i].GetWidth();
                uint32_t OrigTexHeight = _outTex[i].GetHeight();

                ImGui::AlignFirstTextHeightToWidgets();
                ImGui::Text("Native Reso:%dx%d", OrigTexWidth, OrigTexHeight);
                ImGui::SameLine();
                ImGui::Checkbox(("Show " + _texNames[i]).c_str(),
                    &showImage[i]);
                ImGui::Image(tex_id,
                    ImVec2(width, width*OrigTexHeight / OrigTexWidth));
            }
        }
    }
    for (int i = 1; i < kNumTargetTex; ++i) {
        if (showImage[i]) _outTex[i].GuiShow();
    }
    if (_depthMode == kNoDepth && _colorMode == kNoColor) {
        _depthMode = kDepth;
    }
}

void
SensorTexGen::GetColorReso(uint16_t& width, uint16_t& height) const
{
    width = _outTex[kColorTex].GetWidth();
    height = _outTex[kColorTex].GetHeight();
}

void
SensorTexGen::GetDepthInfrareReso(uint16_t& width, uint16_t& height) const
{
    width = _outTex[kDepthTex].GetWidth();
    height = _outTex[kDepthTex].GetHeight();
}

ColorBuffer*
SensorTexGen::GetOutTex(TargetTexture target)
{
    return &_outTex[target];
}

void
SensorTexGen::_RetirePreviousFrameKinectBuffer()
{
    if (_preColorMode == kColor && _pKinectBuf[IRGBDStreamer::kColor]) {
        _pFrameAlloc[IRGBDStreamer::kColor]->DiscardBuffer(
            Graphics::g_stats.lastFrameEndFence,
            _pKinectBuf[IRGBDStreamer::kColor]);
        _pKinectBuf[IRGBDStreamer::kColor] = nullptr;
    }
    if (_preDepthMode != kNoDepth && _pKinectBuf[IRGBDStreamer::kDepth]) {
        _pFrameAlloc[IRGBDStreamer::kDepth]->DiscardBuffer(
            Graphics::g_stats.lastFrameEndFence,
            _pKinectBuf[IRGBDStreamer::kDepth]);
        _pKinectBuf[IRGBDStreamer::kDepth] = nullptr;
        if (_preDepthMode == kDepthWithVisualWithInfrared &&
            _pKinectBuf[IRGBDStreamer::kInfrared]) {
            _pFrameAlloc[IRGBDStreamer::kInfrared]->DiscardBuffer(
                Graphics::g_stats.lastFrameEndFence,
                _pKinectBuf[IRGBDStreamer::kInfrared]);
            _pKinectBuf[IRGBDStreamer::kInfrared] = nullptr;
        }
    }
    _preColorMode = _colorMode;
    _preDepthMode = _depthMode;
}

bool
SensorTexGen::_PrepareAndFillinKinectBuffers()
{
    bool results = true;
    FrameData frames[IRGBDStreamer::kNumBufferTypes];
    if (!_pKinect2->GetNewFrames(frames[IRGBDStreamer::kColor],
        frames[IRGBDStreamer::kDepth], frames[IRGBDStreamer::kInfrared])) {
        results = false;
    }
    if (_colorMode == kColor) {
        results &= _FillinKinectBuffer(
            IRGBDStreamer::kColor, frames[IRGBDStreamer::kColor]);
    }
    if (_depthMode != kNoDepth) {
        results &= _FillinKinectBuffer(
            IRGBDStreamer::kDepth, frames[IRGBDStreamer::kDepth]);
    }
    if (_depthMode == kDepthWithVisualWithInfrared) {
        results &= _FillinKinectBuffer(
            IRGBDStreamer::kInfrared, frames[IRGBDStreamer::kInfrared]);
    }
    return results;
}

bool
SensorTexGen::_FillinKinectBuffer(
    IRGBDStreamer::BufferType bufType, const FrameData& frame)
{
    _pKinectBuf[bufType] = _pFrameAlloc[bufType]->RequestFrameBuffer();
    if (!frame.pData) {
        return false;
    }
    uint8_t* ptr =
        reinterpret_cast<uint8_t*>(_pKinectBuf[bufType]->GetMappedPtr());
    std::memcpy(ptr, frame.pData, frame.Size);
    return true;
}