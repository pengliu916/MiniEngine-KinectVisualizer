#include "pch.h"
#include "SensorTexGen.h"

using namespace DirectX;
using namespace Microsoft::WRL;

#define Bind(ctx, rootIdx, offset, count, resource) \
ctx.SetDynamicDescriptors(rootIdx, offset, count, resource)

#define Trans(ctx, res, state) \
ctx.TransitionResource(res, state)

#define BeginTrans(ctx, res, state) \
ctx.BeginResourceTransition(res, state)

namespace {
using State = D3D12_RESOURCE_STATES;
const State UAV   = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
const State CBV   = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
const State RTV   = D3D12_RESOURCE_STATE_RENDER_TARGET;
const State DSV   = D3D12_RESOURCE_STATE_DEPTH_WRITE;
const State psSRV = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
const State csSRV = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
const State vsSRV = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

const UINT kDepthMode = SensorTexGen::kNumDepthMode;
const UINT kColorMode = SensorTexGen::kNumColorMode;
const UINT kDataMode  = SensorTexGen::kNumDataMode;
const UINT kDSrcMode  = SensorTexGen::kNumSrcMode;

const IRGBDStreamer::BufferType kIDepth = IRGBDStreamer::kDepth;
const IRGBDStreamer::BufferType kIInfrared = IRGBDStreamer::kInfrared;
const IRGBDStreamer::BufferType kIColor = IRGBDStreamer::kColor;

// bools
bool _useCS = true;
bool _streaming = true;
bool _perFrameUpdate = true;
bool _animateFakedDepth = false;
bool _cbStaled = true;
bool _recompie = false;

float _fAnimTOffset = 0.f;
float _fAnimSpeed = 0.2f;

RootSignature _rootSignature;
GraphicsPSO _gfxDepthPSO[kDepthMode][kDataMode][kDSrcMode];
GraphicsPSO _gfxColorPSO[kColorMode][kDataMode];
ComputePSO _cptDepthPSO[kDepthMode][kDataMode][kDSrcMode];
ComputePSO _cptColorPSO[kColorMode][kDataMode];

DXGI_FORMAT RTformat[] = {
    DXGI_FORMAT_R16_UNORM, // For kDepth
    DXGI_FORMAT_R11G11B10_FLOAT, // For kDepthVisualTex
    DXGI_FORMAT_R11G11B10_FLOAT, // For kInfraredTex
    DXGI_FORMAT_R11G11B10_FLOAT, // For kColorTex
};

inline HRESULT _Compile(LPCWSTR shaderName,
    const D3D_SHADER_MACRO* macro, ID3DBlob** bolb)
{
    int iLen = (int)wcslen(shaderName);
    char target[8];
    wchar_t fileName[128];
    swprintf_s(fileName, 128, L"SensorTexGen_%ls.hlsl", shaderName);
    switch (shaderName[iLen - 2])
    {
    case 'c': sprintf_s(target, 8, "cs_5_1"); break;
    case 'p': sprintf_s(target, 8, "ps_5_1"); break;
    case 'v': sprintf_s(target, 8, "vs_5_1"); break;
    default:
        PRINTERROR(L"Shader name: %s is Invalid!", shaderName);
    }
    return Graphics::CompileShaderFromFile(Core::GetAssetFullPath(
        fileName).c_str(), macro,
        D3D_COMPILE_STANDARD_FILE_INCLUDE, "main",
        target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, bolb);
}

void _CreatePSOs(SensorTexGen::ColorMode CMode, SensorTexGen::DepthMode DMode,
    SensorTexGen::ProcessMode PMode, SensorTexGen::DepthSource DSrc)
{
    _recompie = false;
    HRESULT hr;
    ComPtr<ID3DBlob> quadVS;
    ComPtr<ID3DBlob> cpyDPS, cpyCPS, cpyDCS, cpyCCS;
    D3D_SHADER_MACRO macro[] = {
        { "__hlsl", "1" }, // 0
        { "UNDISTORTION", "0" }, // 1
        { "VISUALIZED_DEPTH_TEX", "0" }, // 2
        { "INFRARED_TEX", "0" }, // 3
        { "COLOR_TEX", "0" }, // 4
        { "DEPTH_TEX", "1" }, // 5
        { "DEPTH_SOURCE", "0" }, // 6
        { nullptr, nullptr }
    };
    V(_Compile(L"SingleTriangleQuad_vs", macro, &quadVS));
    macro[1].Definition = PMode == SensorTexGen::kRaw ? "0" : "1";
    UINT numRT = 1;
    switch (DMode) {
    case SensorTexGen::kDepthWithVisual:
        macro[2].Definition = "1"; numRT = 2; break;
    case SensorTexGen::kDepthWithVisualWithInfrared:
        macro[2].Definition = "1"; macro[3].Definition = "1"; numRT = 3; break;
    }
    char tmpChar[8];
    sprintf_s(tmpChar, 8, "%d", DSrc);
    macro[6].Definition = tmpChar;
    V(_Compile(L"Copy_ps", macro, &cpyDPS));
    V(_Compile(L"Copy_cs", macro, &cpyDCS));
    GraphicsPSO& gfxDepthPSO = _gfxDepthPSO[DMode][PMode][DSrc];
    gfxDepthPSO.SetRootSignature(_rootSignature);
    gfxDepthPSO.SetRasterizerState(Graphics::g_RasterizerDefault);
    gfxDepthPSO.SetBlendState(Graphics::g_BlendDisable);
    gfxDepthPSO.SetDepthStencilState(Graphics::g_DepthStateDisabled);
    gfxDepthPSO.SetSampleMask(0xFFFFFFFF);
    gfxDepthPSO.SetInputLayout(0, nullptr);
    gfxDepthPSO.SetPrimitiveTopologyType(
        D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    gfxDepthPSO.SetVertexShader(
        quadVS->GetBufferPointer(), quadVS->GetBufferSize());
    gfxDepthPSO.SetRenderTargetFormats(numRT, RTformat, DXGI_FORMAT_UNKNOWN);
    gfxDepthPSO.SetPixelShader(
        cpyDPS->GetBufferPointer(), cpyDPS->GetBufferSize());
    gfxDepthPSO.Finalize();

    ComputePSO& cptDepthPSO = _cptDepthPSO[DMode][PMode][DSrc];
    cptDepthPSO.SetRootSignature(_rootSignature);
    cptDepthPSO.SetComputeShader(
        cpyDCS->GetBufferPointer(), cpyDCS->GetBufferSize());
    cptDepthPSO.Finalize();

    if (CMode == SensorTexGen::kNoColor) {
        return;
    }
    macro[2].Definition = "0";
    macro[3].Definition = "0";
    macro[4].Definition = "1";
    macro[5].Definition = "0";
    V(_Compile(L"Copy_ps", macro, &cpyCPS));
    V(_Compile(L"Copy_cs", macro, &cpyCCS));
    _gfxColorPSO[CMode][PMode] = gfxDepthPSO;
    GraphicsPSO& gfxColorPSO = _gfxColorPSO[CMode][PMode];
    gfxColorPSO.SetRenderTargetFormats(1, &RTformat[3], DXGI_FORMAT_UNKNOWN);
    gfxColorPSO.SetPixelShader(cpyCPS->GetBufferPointer(), cpyCPS->GetBufferSize());
    gfxColorPSO.Finalize();

    _cptColorPSO[CMode][PMode] = cptDepthPSO;
    ComputePSO& cptColorPSO = _cptColorPSO[CMode][PMode];
    cptColorPSO.SetComputeShader(
        cpyCCS->GetBufferPointer(), cpyCCS->GetBufferSize());
    cptColorPSO.Finalize();
}
}

SensorTexGen::SensorTexGen(
    bool enableColor, bool enableDepth, bool enableInfrared)
{
    _preColorMode = _colorMode;
    _preDepthMode = _depthMode;
    _pKinect2 = StreamFactory::createFromKinect2(
        enableColor, enableDepth, enableInfrared);
    _pKinect2->StartStream();
    _cbKinect.fFgDist = 3.f;
    _cbKinect.fBgDist = 5.f;
}

SensorTexGen::~SensorTexGen()
{
}

void
SensorTexGen::OnCreateResource(LinearAllocator& uploadHeapAlloc)
{
    HRESULT hr = S_OK;
    _cbKinect.u2DepthInfraredReso = DEPTH_RESO;

    _pFrameAlloc[kIDepth] = new LinearFrameAllocator(
        DEPTH_RESO.x * DEPTH_RESO.y, sizeof(uint16_t), DXGI_FORMAT_R16_UINT);
    _pFrameAlloc[kIInfrared] = new LinearFrameAllocator(
        DEPTH_RESO.x * DEPTH_RESO.y, sizeof(uint16_t), DXGI_FORMAT_R16_UINT);

    _depthInfraredViewport = {};
    _depthInfraredViewport.Width = (FLOAT)DEPTH_RESO.x;
    _depthInfraredViewport.Height = (FLOAT)DEPTH_RESO.y;
    _depthInfraredViewport.MaxDepth = 1.0f;
    _depthInfraredScissorRect = {};
    _depthInfraredScissorRect.right = DEPTH_RESO.x;
    _depthInfraredScissorRect.bottom = DEPTH_RESO.y;

    _cbKinect.u2ColorReso = COLOR_RESO;

    _pFrameAlloc[kIColor] = new LinearFrameAllocator(
        COLOR_RESO.x * COLOR_RESO.y, sizeof(uint32_t),
        DXGI_FORMAT_R8G8B8A8_UINT);

    _colorViewport = {};
    _colorViewport.Width = (FLOAT)COLOR_RESO.x;
    _colorViewport.Height = (FLOAT)COLOR_RESO.y;
    _colorViewport.MaxDepth = 1.0f;
    _colorScissorRect = {};
    _colorScissorRect.right = COLOR_RESO.x;
    _colorScissorRect.bottom = COLOR_RESO.y;

    // Create rootsignature
    _rootSignature.Reset(3);
    _rootSignature[0].InitAsConstantBuffer(0);
    _rootSignature[1].InitAsDescriptorRange(
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, IRGBDStreamer::kNumBufferTypes);
    _rootSignature[2].InitAsDescriptorRange(
        D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0, IRGBDStreamer::kNumBufferTypes + 2);
    _rootSignature.Finalize(L"SensorTexGen",
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS);

    // GPU buffer for fake scene camera matrix
    float cameraMatrix[12] = {
        1.f,   0.f,   0.f,   0.f,
        0.f,   1.f,   0.f,   0.f,
        0.f,   0.f,   1.f,   0.f,
    };
    _camMatrixBuf.Create(L"FakeSceneCameraMatrix",
        3, 4 * sizeof(float), (void*)cameraMatrix);

    _gpuCB.Create(L"SensorTexGen_CB", 1, sizeof(RenderCB),
        (void*)&_cbKinect);
    _pUploadCB = new DynAlloc(
        std::move(uploadHeapAlloc.Allocate(sizeof(RenderCB))));
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
    if (_pKinect2) {
        delete _pKinect2;
        _pKinect2 = nullptr;
    }
    delete _pUploadCB;
    _gpuCB.Destroy();
    _camMatrixBuf.Destroy();
}

bool
SensorTexGen::OnRender(CommandContext& cmdCtx, ColorBuffer* pDepthOut,
    ColorBuffer* pColorOut, ColorBuffer* pInfraredOut,
    ColorBuffer* pDepthVisOut)
{
    static float fAnimTime = 0;
    if (_depthSource != kKinect) {
        if (_animateFakedDepth) {
            fAnimTime += static_cast<float>(Core::g_deltaTime) * _fAnimSpeed;
        }
        _cbKinect.fTime = fAnimTime + _fAnimTOffset;
        _cbStaled = true;
    }
    if (!_streaming) {
        return false;
    }
    if (_cbStaled) {
        memcpy(_pUploadCB->DataPtr, &_cbKinect, sizeof(RenderCB));
        cmdCtx.CopyBufferRegion(_gpuCB, 0, _pUploadCB->Buffer,
            _pUploadCB->Offset, sizeof(RenderCB));
        Trans(cmdCtx, _gpuCB, CBV);
        _cbStaled = false;
    }
    // retire Kinect buffers used for previous frame
    _RetirePreviousFrameKinectBuffer();
    // prepare and fill in new data from Kinect
    bool needUpdate = _PrepareAndFillinKinectBuffers() || _perFrameUpdate;
    // Render to screen
    if (_useCS && needUpdate) {
        ComputeContext& cptCtx = cmdCtx.GetComputeContext();
        GPU_PROFILE(cptCtx, L"Read&Present RGBD");

        cptCtx.SetRootSignature(_rootSignature);
        cptCtx.SetConstantBuffer(0, _gpuCB.RootConstantBufferView());

        if (_depthSource != kKinect) {
            Trans(cptCtx, _camMatrixBuf, UAV);
            Bind(cptCtx, 2, 4, 1, &_camMatrixBuf.GetUAV());
        }
        if (_depthMode != kNoDepth && pDepthOut) {
            Bind(cptCtx, 1, 0, 1, &_pKinectBuf[kIDepth]->GetSRV());
            Bind(cptCtx, 2, 0, 1, &pDepthOut->GetUAV());
            Trans(cptCtx, *pDepthOut, UAV);
            if (_depthMode != kDepth && pDepthVisOut) {
                Bind(cptCtx, 2, 1, 1, &pDepthVisOut->GetUAV());
                Trans(cptCtx, *pDepthVisOut, UAV);
            }
        }
        if (_depthMode == kDepthWithVisualWithInfrared && pInfraredOut) {
            Bind(cptCtx, 1, 1, 1, &_pKinectBuf[kIInfrared]->GetSRV());
            Bind(cptCtx, 2, 2, 1, &pInfraredOut->GetUAV());
            Trans(cptCtx, *pInfraredOut, UAV);
        }
        if (_colorMode == kColor && pColorOut) {
            Bind(cptCtx, 1, 2, 1, &_pKinectBuf[kIColor]->GetSRV());
            Bind(cptCtx, 2, 3, 1, &pColorOut->GetUAV());
            Trans(cptCtx, *pColorOut, UAV);
        }
        if (_depthMode != kNoDepth &&
            (pDepthOut || pDepthVisOut || pInfraredOut)) {
            ComputePSO& cptPSO =
                _cptDepthPSO[_depthMode][_processMode][_depthSource];
            if (_recompie || !cptPSO.GetPipelineStateObject()) {
                _CreatePSOs(_colorMode, _depthMode, _processMode, _depthSource);
            }
            cptCtx.SetPipelineState(cptPSO);
            cptCtx.Dispatch1D(pDepthOut->GetWidth() *
                pDepthOut->GetHeight(), THREAD_PER_GROUP);
        }
        if (_colorMode != kNoColor && pColorOut) {
            ComputePSO& cptPSO = _cptColorPSO[_colorMode][_processMode];
            if (_recompie || !cptPSO.GetPipelineStateObject()) {
                _CreatePSOs(_colorMode, _depthMode, _processMode, _depthSource);
            }
            cptCtx.SetPipelineState(cptPSO);
            cptCtx.Dispatch1D(pColorOut->GetWidth() *
                pColorOut->GetHeight(), THREAD_PER_GROUP);
        }
    } else if (needUpdate) {
        GraphicsContext& gfxCtx = cmdCtx.GetGraphicsContext();
        GPU_PROFILE(gfxCtx, L"Read&Present RGBD");

        gfxCtx.SetRootSignature(_rootSignature);
        gfxCtx.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        gfxCtx.SetConstantBuffer(0, _gpuCB.RootConstantBufferView());

        if (_depthSource != kKinect) {
            Trans(gfxCtx, _camMatrixBuf, UAV);
            Bind(gfxCtx, 2, 4, 1, &_camMatrixBuf.GetUAV());
        }

        if (_depthMode != kNoDepth && pDepthOut) {
            Bind(gfxCtx, 1, 0, 1, &_pKinectBuf[kIDepth]->GetSRV());
            Trans(gfxCtx, *pDepthOut, RTV);
            if (_depthMode != kDepth && pDepthVisOut) {
                Trans(gfxCtx, *pDepthVisOut, RTV);
            }
        }
        if (_depthMode == kDepthWithVisualWithInfrared && pInfraredOut) {
            Bind(gfxCtx, 1, 1, 1, &_pKinectBuf[kIInfrared]->GetSRV());
            Trans(gfxCtx, *pInfraredOut, RTV);
        }
        if (_colorMode == kColor && pColorOut) {
            Bind(gfxCtx, 1, 2, 1, &_pKinectBuf[kIColor]->GetSRV());
            Trans(gfxCtx, *pColorOut, RTV);
        }
        if (_depthMode != kNoDepth &&
            (pDepthOut || pDepthVisOut || pInfraredOut)) {
            static D3D12_CPU_DESCRIPTOR_HANDLE RTVs[3];
            RTVs[0] = pDepthOut->GetRTV();
            RTVs[1] = pDepthVisOut->GetRTV();
            RTVs[2] = pInfraredOut->GetRTV();
            gfxCtx.SetRenderTargets(_depthMode + 1, RTVs);
            gfxCtx.SetViewports(1, &_depthInfraredViewport);
            gfxCtx.SetScisors(1, &_depthInfraredScissorRect);
            GraphicsPSO& gfxPSO =
                _gfxDepthPSO[_depthMode][_processMode][_depthSource];
            if (_recompie || !gfxPSO.GetPipelineStateObject()) {
                _CreatePSOs(_colorMode, _depthMode, _processMode, _depthSource);
            }
            gfxCtx.SetPipelineState(gfxPSO);
            gfxCtx.Draw(3);
        }
        if (_colorMode != kNoColor && pColorOut)
        {
            gfxCtx.SetRenderTarget(pColorOut->GetRTV());
            gfxCtx.SetViewports(1, &_colorViewport);
            gfxCtx.SetScisors(1, &_colorScissorRect);
            GraphicsPSO& gfxPSO = _gfxColorPSO[_colorMode][_processMode];
            if (_recompie || !gfxPSO.GetPipelineStateObject()) {
                _CreatePSOs(_colorMode, _depthMode, _processMode, _depthSource);
            }
            gfxCtx.SetPipelineState(gfxPSO);
            gfxCtx.Draw(3);
        }
    }
    return needUpdate;
}

void
SensorTexGen::RenderGui()
{
#define M(x) _cbStaled |= x
    using namespace ImGui;
    static bool showImage[kNumTargetTex] = {};
    if (CollapsingHeader("SensorTexGen")) {
        if (Button("RecompileShaders##SensorTexGen")) {
            _recompie = true;
        }
        Separator();
        Text("DepthMap Source:");
        RadioButton("Kinect##Src", (int*)&_depthSource, 0); SameLine();
        RadioButton("Procedual##Src", (int*)&_depthSource, 1); SameLine();
        RadioButton("Simple##Src", (int*)&_depthSource, 2);
        Checkbox("Animate", &_animateFakedDepth);
        if (_depthSource != kKinect) {
            SameLine();
            if (!_animateFakedDepth) {
                M(DragFloat("TimeSlider", &_fAnimTOffset, 0.1f));
            } else {
                M(SliderFloat("AnimSpeed", &_fAnimSpeed, 0.1f, 2.f));
            }
        }
        if (_depthSource == kSimple) {
            M(SliderFloat("BGDist", &_cbKinect.fBgDist, 0.5f, 5.f));
            M(SliderFloat("FGDist", &_cbKinect.fFgDist, 0.5f, 5.f));
        }
        Separator();
        Checkbox("Streaming Data", &_streaming);
        Checkbox("Update EveryFrame", &_perFrameUpdate);
        Checkbox("Use Compute Shader", &_useCS);
        Checkbox("Use Undistortion", (bool*)&_processMode);
        Separator();
        RadioButton("No Depth", (int*)&_depthMode, -1);
        RadioButton("Depth", (int*)&_depthMode, 0);
        RadioButton("Depth&Visual", (int*)&_depthMode, 1);
        RadioButton("Depth&Visual&Infrared", (int*)&_depthMode, 2);
        Separator();
        RadioButton("No Color", (int*)&_colorMode, -1);
        RadioButton("Color", (int*)&_colorMode, 0);
    }
    if (_depthMode == kNoDepth && _colorMode == kNoColor) {
        _depthMode = kDepth;
    }
#undef M
}

StructuredBuffer*
SensorTexGen::GetVCamMatrixBuf()
{
    return &_camMatrixBuf;
}

void
SensorTexGen::_RetirePreviousFrameKinectBuffer()
{
    if (_preColorMode == kColor && _pKinectBuf[kIColor]) {
        _pFrameAlloc[kIColor]->DiscardBuffer(
            Graphics::g_stats.lastFrameEndFence,
            _pKinectBuf[kIColor]);
        _pKinectBuf[kIColor] = nullptr;
    }
    if (_preDepthMode != kNoDepth && _pKinectBuf[kIDepth]) {
        _pFrameAlloc[kIDepth]->DiscardBuffer(
            Graphics::g_stats.lastFrameEndFence,
            _pKinectBuf[kIDepth]);
        _pKinectBuf[kIDepth] = nullptr;
        if (_preDepthMode == kDepthWithVisualWithInfrared &&
            _pKinectBuf[kIInfrared]) {
            _pFrameAlloc[kIInfrared]->DiscardBuffer(
                Graphics::g_stats.lastFrameEndFence,
                _pKinectBuf[kIInfrared]);
            _pKinectBuf[kIInfrared] = nullptr;
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
    if (!_pKinect2->GetNewFrames(frames[kIColor],
        frames[kIDepth], frames[kIInfrared])) {
        results = false;
    }
    if (_colorMode == kColor) {
        results &= _FillinKinectBuffer(kIColor, frames[kIColor]);
    }
    if (_depthMode != kNoDepth) {
        results &= _FillinKinectBuffer(kIDepth, frames[kIDepth]);
    }
    if (_depthMode == kDepthWithVisualWithInfrared) {
        results &= _FillinKinectBuffer(kIInfrared, frames[kIInfrared]);
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