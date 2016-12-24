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
typedef D3D12_RESOURCE_STATES State;
const State UAV   = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
const State RTV   = D3D12_RESOURCE_STATE_RENDER_TARGET;
const State DSV   = D3D12_RESOURCE_STATE_DEPTH_WRITE;
const State psSRV = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
const State csSRV = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
const State vsSRV = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

const UINT kDepthMode = SensorTexGen::kNumDepthMode;
const UINT kColorMode = SensorTexGen::kNumColorMode;
const UINT kDataMode  = SensorTexGen::kNumDataMode;

const IRGBDStreamer::BufferType kIDepth = IRGBDStreamer::kDepth;
const IRGBDStreamer::BufferType kIInfrared = IRGBDStreamer::kInfrared;
const IRGBDStreamer::BufferType kIColor = IRGBDStreamer::kColor;

// bools
bool _useCS = true;
bool _streaming = true;
bool _perFrameUpdate = true;
bool _fakeDepthMap = false;
bool _animateFakedDepth = false;
bool _cbStaled = true;

std::string _texNames[SensorTexGen::kNumTargetTex] =
{"Depth Raw","Depth Visualized","Infrared Gamma","Color Raw"};

RootSignature _rootSignature;
// last index indicate whether use faked depthmap
GraphicsPSO _gfxDepthPSO[kDepthMode][kDataMode][2];
GraphicsPSO _gfxColorPSO[kColorMode][kDataMode];
// last index indicate whether use faked depthmap
ComputePSO _cptDepthPSO[kDepthMode][kDataMode][2];
ComputePSO _cptColorPSO[kColorMode][kDataMode];

DXGI_FORMAT RTformat[] = {
    DXGI_FORMAT_R16_UINT, // For kDepth
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

void _CreatePSOs()
{
    HRESULT hr;
    // Create PSO
    ComPtr<ID3DBlob> quadVS;
    // last index indicate whether use faked depthmap
    ComPtr<ID3DBlob> copyDepthPS[kDepthMode][kDataMode][2];
    ComPtr<ID3DBlob> copyColorPS[kColorMode][kDataMode];
    // last index indicate whether use faked depthmap
    ComPtr<ID3DBlob> copyDepthCS[kDepthMode][kDataMode][2];
    ComPtr<ID3DBlob> copyColorCS[kColorMode][kDataMode];
    uint32_t compileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    D3D_SHADER_MACRO macro[] = {
        {"__hlsl", "1"}, // 0
        {"UNDISTORTION", "0"}, // 1
        {"VISUALIZED_DEPTH_TEX", "0"}, // 2
        {"INFRARED_TEX", "0"}, // 3
        {"COLOR_TEX", "0"}, // 4
        {"FAKEDEPTH", "0"}, // 5
        {nullptr, nullptr}
    };
    V(_Compile(L"SingleTriangleQuad_vs", macro, &quadVS));

    char tempChar[8];
    for (int i = 0; i < kDataMode; ++i) {
        sprintf_s(tempChar, 8, "%d", i);
        macro[1].Definition = tempChar; // UNDISTORTION
        for (int j = 0; j < kDepthMode; ++j) {
            switch (j) {
            case SensorTexGen::kDepthWithVisualWithInfrared:
                macro[3].Definition = "1"; // INFRARED_TEX
            case SensorTexGen::kDepthVisualTex:
                macro[2].Definition = "1"; // VISUALIZED_DEPTH_TEX
            }
            V(_Compile(L"Copy_ps", macro, &copyDepthPS[j][i][0]));
            V(_Compile(L"Copy_cs", macro, &copyDepthCS[j][i][0]));
            macro[5].Definition = "1";
            V(_Compile(L"Copy_ps", macro, &copyDepthPS[j][i][1]));
            V(_Compile(L"Copy_cs", macro, &copyDepthCS[j][i][1]));
            macro[5].Definition = "0";
            macro[2].Definition = "0"; // VISUALIZED_DEPTH_TEX
            macro[3].Definition = "0"; // INFRARED_TEX
        }
        macro[4].Definition = "1"; // COLOR_TEX
        V(_Compile(L"Copy_ps", macro, &copyColorPS[0][i]));
        V(_Compile(L"Copy_cs", macro, &copyColorCS[0][i]));
        macro[4].Definition = "0"; // COLOR_TEX
    }

    for (int i = 0; i < kDataMode; ++i) {
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
            1, &RTformat[3], DXGI_FORMAT_UNKNOWN);
        _gfxColorPSO[0][i].SetPixelShader(copyColorPS[0][i]->GetBufferPointer(),
            copyColorPS[0][i]->GetBufferSize());
        _cptColorPSO[0][i].SetRootSignature(_rootSignature);
        _cptColorPSO[0][i].SetComputeShader(
            copyColorCS[0][i]->GetBufferPointer(),
            copyColorCS[0][i]->GetBufferSize());
        _cptColorPSO[0][i].Finalize();

        for (int j = 0; j < kDepthMode; ++j) {
            _gfxDepthPSO[j][i][0] = _gfxColorPSO[0][i];
            _gfxDepthPSO[j][i][0].SetPixelShader(
                copyDepthPS[j][i][0]->GetBufferPointer(),
                copyDepthPS[j][i][0]->GetBufferSize());
            _gfxDepthPSO[j][i][0].SetRenderTargetFormats(
                j + 1, RTformat, DXGI_FORMAT_UNKNOWN);
            _gfxDepthPSO[j][i][0].Finalize();

            _gfxDepthPSO[j][i][1] = _gfxColorPSO[0][i];
            _gfxDepthPSO[j][i][1].SetPixelShader(
                copyDepthPS[j][i][1]->GetBufferPointer(),
                copyDepthPS[j][i][1]->GetBufferSize());
            _gfxDepthPSO[j][i][1].SetRenderTargetFormats(
                j + 1, RTformat, DXGI_FORMAT_UNKNOWN);
            _gfxDepthPSO[j][i][1].Finalize();

            _cptDepthPSO[j][i][0].SetRootSignature(_rootSignature);
            _cptDepthPSO[j][i][0].SetComputeShader(
                copyDepthCS[j][i][0]->GetBufferPointer(),
                copyDepthCS[j][i][0]->GetBufferSize());
            _cptDepthPSO[j][i][0].Finalize();

            _cptDepthPSO[j][i][1].SetRootSignature(_rootSignature);
            _cptDepthPSO[j][i][1].SetComputeShader(
                copyDepthCS[j][i][1]->GetBufferPointer(),
                copyDepthCS[j][i][1]->GetBufferSize());
            _cptDepthPSO[j][i][1].Finalize();
        }
        _gfxColorPSO[0][i].Finalize();
    }
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
    _cbKinect.f4S = float4(0.f, 0.f, 3.f, 0.5f);
    _cbKinect.fBgDist = 5.f;
}

SensorTexGen::~SensorTexGen()
{
}

void
SensorTexGen::OnCreateResource(LinearAllocator& uploadHeapAlloc)
{
    HRESULT hr = S_OK;
    uint16_t depWidth, depHeight;
    _pKinect2->GetDepthReso(depWidth, depHeight);

    _cbKinect.f2DepthInfraredReso = XMFLOAT2(depWidth, depHeight);

    _pFrameAlloc[kIDepth] = new LinearFrameAllocator(
        depWidth * depHeight, sizeof(uint16_t), DXGI_FORMAT_R16_UINT);
    _outTex[kDepthVisualTex].Create(L"Depth Visual Tex",
        depWidth, depHeight, 1, DXGI_FORMAT_R11G11B10_FLOAT);
    _outTex[kDepthTex].Create(L"Depth Raw Tex",
        depWidth, depHeight, 1, DXGI_FORMAT_R16_UINT);

    _pFrameAlloc[kIInfrared] = new LinearFrameAllocator(
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

    _pFrameAlloc[kIColor] = new LinearFrameAllocator(
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

    _CreatePSOs();
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
    for (int i = 0; i < kNumTargetTex; ++i) {
        _outTex[i].Destroy();
    }
    if (_pKinect2) {
        delete _pKinect2;
        _pKinect2 = nullptr;
    }
    delete _pUploadCB;
    _gpuCB.Destroy();
}

bool
SensorTexGen::OnRender(CommandContext& EngineContext)
{
    static float fAnimTime = 0;
    if (_animateFakedDepth) {
        fAnimTime += static_cast<float>(Core::g_deltaTime);
        float fx = sin(fAnimTime * 0.5f) * 0.8f;
        float fy = cos(fAnimTime * 0.5f) * 0.8f;
        _cbKinect.f4S.x = fx;
        _cbKinect.f4S.y = fy;
        _cbStaled = true;
    }
    if (!_streaming) {
        return false;
    }
    if (_cbStaled) {
        memcpy(_pUploadCB->DataPtr, &_cbKinect, sizeof(RenderCB));
        EngineContext.CopyBufferRegion(_gpuCB, 0, _pUploadCB->Buffer,
            _pUploadCB->Offset, sizeof(RenderCB));
        _cbStaled = false;
    }
    // retire Kinect buffers used for previous frame
    _RetirePreviousFrameKinectBuffer();
    // prepare and fill in new data from Kinect
    bool needUpdate = _PrepareAndFillinKinectBuffers() || _perFrameUpdate;
    // Render to screen
    if (_useCS && needUpdate) {
        ComputeContext& cptCtx = EngineContext.GetComputeContext();
        GPU_PROFILE(cptCtx, L"Read&Present RGBD");

        cptCtx.SetRootSignature(_rootSignature);
        cptCtx.SetConstantBuffer(0, _gpuCB.RootConstantBufferView());

        if (_depthMode != kNoDepth) {
            Bind(cptCtx, 1, 0, 1, &_pKinectBuf[kIDepth]->GetSRV());
            Bind(cptCtx, 2, 0, 1, &_outTex[kDepthTex].GetUAV());
            Trans(cptCtx, _outTex[kDepthTex], UAV);
            if (_depthMode != kDepth) {
                Bind(cptCtx, 2, 1, 1, &_outTex[kDepthVisualTex].GetUAV());
                Trans(cptCtx, _outTex[kDepthVisualTex], UAV);
            }
        }
        if (_depthMode == kDepthWithVisualWithInfrared) {
            Bind(cptCtx, 1, 1, 1, &_pKinectBuf[kIInfrared]->GetSRV());
            Bind(cptCtx, 2, 2, 1, &_outTex[kInfraredTex].GetUAV());
            Trans(cptCtx, _outTex[kInfraredTex], UAV);
        }
        if (_colorMode == kColor) {
            Bind(cptCtx, 1, 2, 1, &_pKinectBuf[kIColor]->GetSRV());
            Bind(cptCtx, 2, 3, 1, &_outTex[kColorTex].GetUAV());
            Trans(cptCtx, _outTex[kColorTex], UAV);
        }
        cptCtx.SetPipelineState(
            _cptDepthPSO[_depthMode][_processMode][_fakeDepthMap]);
        cptCtx.Dispatch1D(_outTex[kDepthTex].GetWidth() *
            _outTex[kDepthTex].GetHeight(), THREAD_PER_GROUP);

        if (_colorMode != kNoColor) {
            cptCtx.SetPipelineState(_cptColorPSO[_colorMode][_processMode]);
            cptCtx.Dispatch1D(_outTex[kColorTex].GetWidth() *
                _outTex[kColorTex].GetHeight(), THREAD_PER_GROUP);
        }
    } else if (needUpdate) {
        GraphicsContext& gfxCtx = EngineContext.GetGraphicsContext();
        GPU_PROFILE(gfxCtx, L"Read&Present RGBD");

        gfxCtx.SetRootSignature(_rootSignature);
        gfxCtx.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        gfxCtx.SetConstantBuffer(0, _gpuCB.RootConstantBufferView());

        if (_depthMode != kNoDepth) {
            Bind(gfxCtx, 1, 0, 1, &_pKinectBuf[kIDepth]->GetSRV());
            Trans(gfxCtx, _outTex[kDepthTex], RTV);
            if (_depthMode != kDepth) {
                Trans(gfxCtx, _outTex[kDepthVisualTex], RTV);
            }
        }
        if (_depthMode == kDepthWithVisualWithInfrared) {
            Bind(gfxCtx, 1, 1, 1, &_pKinectBuf[kIInfrared]->GetSRV());
            Trans(gfxCtx, _outTex[kInfraredTex], RTV);
        }
        if (_colorMode == kColor) {
            Bind(gfxCtx, 1, 2, 1, &_pKinectBuf[kIColor]->GetSRV());
            Trans(gfxCtx, _outTex[kColorTex], RTV);
        }

        gfxCtx.SetRenderTargets(_depthMode + 1, _outTextureRTV);
        gfxCtx.SetViewports(1, &_depthInfraredViewport);
        gfxCtx.SetScisors(1, &_depthInfraredScissorRect);
        gfxCtx.SetPipelineState(
            _gfxDepthPSO[_depthMode][_processMode][_fakeDepthMap]);
        gfxCtx.Draw(3);

        gfxCtx.SetRenderTargets(1, &_outTextureRTV[kColorTex]);
        gfxCtx.SetViewports(1, &_colorViewport);
        gfxCtx.SetScisors(1, &_colorScissorRect);
        if (_colorMode != kNoColor)
        {
            gfxCtx.SetPipelineState(_gfxColorPSO[0][_processMode]);
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
    if (CollapsingHeader("Sensor Tex Generator")) {
        if (Button("Recompile Shaders")) {
            _CreatePSOs();
        }
        Separator();
        Checkbox("Use Faked Depth", &_fakeDepthMap);
        if (_fakeDepthMap) {
            Checkbox("Animate", &_animateFakedDepth);
            M(SliderFloat("BGDist", &_cbKinect.fBgDist, 0.5f, 5.f));
            M(SliderFloat("FGDist", &_cbKinect.f4S.z, 0.5f, 5.f));
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

        Indent();
        float width = GetContentRegionAvail().x;
        for (int i = 1; i < kNumTargetTex; ++i) {
            if (!CollapsingHeader(_texNames[i].c_str())) {
                continue;
            }
            ImTextureID tex_id = (void*)&_outTex[i].GetSRV();
            uint32_t OrigTexWidth = _outTex[i].GetWidth();
            uint32_t OrigTexHeight = _outTex[i].GetHeight();

            AlignFirstTextHeightToWidgets();
            Text("Native Reso:%dx%d", OrigTexWidth, OrigTexHeight);
            SameLine();
            Checkbox(("Show " + _texNames[i]).c_str(), &showImage[i]);
            Image(tex_id, ImVec2(width, width*OrigTexHeight / OrigTexWidth));
        }
        Unindent();
    }
    for (int i = 1; i < kNumTargetTex; ++i) {
        if (showImage[i]) _outTex[i].GuiShow();
    }
    if (_depthMode == kNoDepth && _colorMode == kNoColor) {
        _depthMode = kDepth;
    }
#undef M
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