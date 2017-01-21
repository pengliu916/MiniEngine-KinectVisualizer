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
const State CBV   = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
const State RTV   = D3D12_RESOURCE_STATE_RENDER_TARGET;
const State DSV   = D3D12_RESOURCE_STATE_DEPTH_WRITE;
const State psSRV = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
const State csSRV = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
const State vsSRV = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

const UINT kDepthMode = SensorTexGen::kNumDepthMode;
const UINT kColorMode = SensorTexGen::kNumColorMode;
const UINT kDataMode  = SensorTexGen::kNumDataMode;

const SensorTexGen::BufferType kIDepth = SensorTexGen::kCamDepth;
const SensorTexGen::BufferType kIInfrared = SensorTexGen::kCamInfrared;
const SensorTexGen::BufferType kIColor = SensorTexGen::kCamColor;

// bools
bool _useCS = true;
bool _streaming = true;
bool _perFrameUpdate = true;
bool _fakeDepthMap = true;
bool _animateFakedDepth = true;
bool _cbStaled = true;

RootSignature _rootSignature;
// last index indicate whether use faked depthmap
GraphicsPSO _gfxDepthPSO[kDepthMode][kDataMode][2];
GraphicsPSO _gfxColorPSO[kColorMode][kDataMode];
// last index indicate whether use faked depthmap
ComputePSO _cptDepthPSO[kDepthMode][kDataMode][2];
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
        {"DEPTH_TEX", "0"}, // 5
        {"FAKEDDEPTH", "0"}, // 6
        {nullptr, nullptr}
    };
    V(_Compile(L"SingleTriangleQuad_vs", macro, &quadVS));

    char tempChar[8];
    for (int i = 0; i < kDataMode; ++i) {
        sprintf_s(tempChar, 8, "%d", i);
        macro[1].Definition = tempChar; // UNDISTORTION
        macro[5].Definition = "1";
        for (int j = 0; j < kDepthMode; ++j) {
            switch (j) {
            case SensorTexGen::kDepthWithVisualWithInfrared:
                macro[3].Definition = "1"; // INFRARED_TEX
            case SensorTexGen::kDepthVisualTex:
                macro[2].Definition = "1"; // VISUALIZED_DEPTH_TEX
            }
            V(_Compile(L"Copy_ps", macro, &copyDepthPS[j][i][0]));
            V(_Compile(L"Copy_cs", macro, &copyDepthCS[j][i][0]));
            macro[6].Definition = "1";
            V(_Compile(L"Copy_ps", macro, &copyDepthPS[j][i][1]));
            V(_Compile(L"Copy_cs", macro, &copyDepthCS[j][i][1]));
            macro[6].Definition = "0";
            macro[2].Definition = "0"; // VISUALIZED_DEPTH_TEX
            macro[3].Definition = "0"; // INFRARED_TEX
        }
        macro[5].Definition = "0";
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
    _cbKinect.u2DepthInfraredReso = DEPTH_RESO;

    _depthInfraredViewport = {};
    _depthInfraredViewport.Width = (FLOAT)DEPTH_RESO.x;
    _depthInfraredViewport.Height = (FLOAT)DEPTH_RESO.y;
    _depthInfraredViewport.MaxDepth = 1.0f;
    _depthInfraredScissorRect = {};
    _depthInfraredScissorRect.right = DEPTH_RESO.x;
    _depthInfraredScissorRect.bottom = DEPTH_RESO.y;

    _cbKinect.u2ColorReso = COLOR_RESO;

    _colorViewport = {};
    _colorViewport.Width = (FLOAT)COLOR_RESO.x;
    _colorViewport.Height = (FLOAT)COLOR_RESO.y;
    _colorViewport.MaxDepth = 1.0f;
    _colorScissorRect = {};
    _colorScissorRect.right = COLOR_RESO.x;
    _colorScissorRect.bottom = COLOR_RESO.y;

    // Create rootsignature
    _rootSignature.Reset(2);
    _rootSignature[0].InitAsConstantBuffer(0);
    _rootSignature[1].InitAsDescriptorRange(
        D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0, kNumBufferTypes + 1);
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
    delete _pUploadCB;
    _gpuCB.Destroy();
}

bool
SensorTexGen::OnRender(CommandContext& cmdCtx, ColorBuffer* pDepthOut,
    ColorBuffer* pColorOut, ColorBuffer* pInfraredOut,
    ColorBuffer* pDepthVisOut)
{
    static float fAnimTime = 0;
    if (_animateFakedDepth) {
        fAnimTime += 0.03f;
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
        cmdCtx.CopyBufferRegion(_gpuCB, 0, _pUploadCB->Buffer,
            _pUploadCB->Offset, sizeof(RenderCB));
        Trans(cmdCtx, _gpuCB, CBV);
        _cbStaled = false;
    }
    // Render to screen
    if (_useCS) {
        ComputeContext& cptCtx = cmdCtx.GetComputeContext();
        GPU_PROFILE(cptCtx, L"Read&Present RGBD");

        cptCtx.SetRootSignature(_rootSignature);
        cptCtx.SetConstantBuffer(0, _gpuCB.RootConstantBufferView());

        if (_depthMode != kNoDepth && pDepthOut) {
            Bind(cptCtx, 1, 0, 1, &pDepthOut->GetUAV());
            Trans(cptCtx, *pDepthOut, UAV);
            if (_depthMode != kDepth && pDepthVisOut) {
                Bind(cptCtx, 1, 1, 1, &pDepthVisOut->GetUAV());
                Trans(cptCtx, *pDepthVisOut, UAV);
            }
        }
        if (_depthMode == kDepthWithVisualWithInfrared && pInfraredOut) {
            Bind(cptCtx, 1, 2, 1, &pInfraredOut->GetUAV());
            Trans(cptCtx, *pInfraredOut, UAV);
        }
        if (_colorMode == kColor && pColorOut) {
            Bind(cptCtx, 1, 3, 1, &pColorOut->GetUAV());
            Trans(cptCtx, *pColorOut, UAV);
        }
        if (_depthMode != kNoDepth &&
            (pDepthOut || pDepthVisOut || pInfraredOut)) {
            cptCtx.SetPipelineState(
                _cptDepthPSO[_depthMode][_processMode][_fakeDepthMap]);
            cptCtx.Dispatch1D(pDepthOut->GetWidth() *
                pDepthOut->GetHeight(), THREAD_PER_GROUP);
        }
        if (_colorMode != kNoColor && pColorOut) {
            cptCtx.SetPipelineState(_cptColorPSO[_colorMode][_processMode]);
            cptCtx.Dispatch1D(pColorOut->GetWidth() *
                pColorOut->GetHeight(), THREAD_PER_GROUP);
        }
    } else {
        GraphicsContext& gfxCtx = cmdCtx.GetGraphicsContext();
        GPU_PROFILE(gfxCtx, L"Read&Present RGBD");

        gfxCtx.SetRootSignature(_rootSignature);
        gfxCtx.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        gfxCtx.SetConstantBuffer(0, _gpuCB.RootConstantBufferView());

        if (_depthMode != kNoDepth && pDepthOut) {
            Trans(gfxCtx, *pDepthOut, RTV);
            if (_depthMode != kDepth && pDepthVisOut) {
                Trans(gfxCtx, *pDepthVisOut, RTV);
            }
        }
        if (_depthMode == kDepthWithVisualWithInfrared && pInfraredOut) {
            Trans(gfxCtx, *pInfraredOut, RTV);
        }
        if (_colorMode == kColor && pColorOut) {
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
            gfxCtx.SetPipelineState(
                _gfxDepthPSO[_depthMode][_processMode][_fakeDepthMap]);
            gfxCtx.Draw(3);
        }
        if (_colorMode != kNoColor && pColorOut)
        {
            gfxCtx.SetRenderTarget(pColorOut->GetRTV());
            gfxCtx.SetViewports(1, &_colorViewport);
            gfxCtx.SetScisors(1, &_colorScissorRect);
            gfxCtx.SetPipelineState(_gfxColorPSO[0][_processMode]);
            gfxCtx.Draw(3);
        }
    }
    return true;
}

void
SensorTexGen::RenderGui()
{
#define M(x) _cbStaled |= x
    using namespace ImGui;
    static bool showImage[kNumTargetTex] = {};
    if (CollapsingHeader("SensorTexGen")) {
        if (Button("RecompileShaders##SensorTexGen")) {
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
    }
    if (_depthMode == kNoDepth && _colorMode == kNoColor) {
        _depthMode = kDepth;
    }
#undef M
}