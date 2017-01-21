#include "pch.h"
#include "TSDFVolume.h"
#include "CalibData.inl"
#include <array>

using namespace DirectX;
using namespace Microsoft::WRL;
using namespace std;

#define frand() ((float)rand()/RAND_MAX)
#define Bind(ctx, rootIdx, offset, count, resource) \
ctx.SetDynamicDescriptors(rootIdx, offset, count, resource)

#define BindCB(ctx, rootIdx, size, ptr) \
ctx.SetDynamicConstantBufferView(rootIdx, size, ptr)

#define Trans(ctx, res, state) \
ctx.TransitionResource(res, state)

#define BeginTrans(ctx, res, state) \
ctx.BeginResourceTransition(res, state)

namespace {
typedef D3D12_RESOURCE_STATES State;
const State UAV   = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
const State RTV   = D3D12_RESOURCE_STATE_RENDER_TARGET;
const State DSV   = D3D12_RESOURCE_STATE_DEPTH_WRITE;
const State IARG  = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
const State psSRV = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
const State csSRV = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
const State vsSRV = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

const UINT kBuf    = ManagedBuf::kNumType;
const UINT kStruct = TSDFVolume::kNumStruct;
const UINT kFilter = TSDFVolume::kNumFilter;
const UINT kTG     = TSDFVolume::kTG;

const UINT numSRVs = 4;
const UINT numUAVs = 7;

const DXGI_FORMAT _stepInfoTexFormat = DXGI_FORMAT_R16G16_FLOAT;

bool _typedLoadSupported = false;

bool _useStepInfoTex = true;
bool _stepInfoDebug = false;
bool _blockVolumeUpdate = true;
bool _readBackGPUBufStatus = true;
bool _noInstance = false;

bool _cbStaled = true;

// define the geometry for a triangle.
const XMFLOAT3 cubeVertices[] = {
    {XMFLOAT3(-0.5f, -0.5f, -0.5f)},{XMFLOAT3(-0.5f, -0.5f,  0.5f)},
    {XMFLOAT3(-0.5f,  0.5f, -0.5f)},{XMFLOAT3(-0.5f,  0.5f,  0.5f)},
    {XMFLOAT3(0.5f, -0.5f, -0.5f)},{XMFLOAT3(0.5f, -0.5f,  0.5f)},
    {XMFLOAT3(0.5f,  0.5f, -0.5f)},{XMFLOAT3(0.5f,  0.5f,  0.5f)},
};
// the index buffer for triangle strip
const uint16_t cubeTrianglesStripIndices[CUBE_TRIANGLESTRIP_LENGTH] = {
    6, 4, 2, 0, 1, 4, 5, 7, 1, 3, 2, 7, 6, 4
};
// the index buffer for cube wire frame (0xffff is the cut value set later)
const uint16_t cubeLineStripIndices[CUBE_LINESTRIP_LENGTH] = {
    0, 1, 5, 4, 0, 2, 3, 7, 6, 2, 0xffff, 6, 4, 0xffff, 7, 5, 0xffff, 3, 1
};

std::once_flag _psoCompiled_flag;
RootSignature _rootsig;
std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numUAVs> _nullUAVs;
std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numSRVs> _nullSRVs;

// PSO for naive updating voxels
ComputePSO _cptUpdatePSO[kBuf][kStruct];
// PSOs for block voxel updating
ComputePSO _cptBlockVolUpdate_Pass1;
ComputePSO _cptBlockVolUpdate_Pass2;
ComputePSO _cptBlockVolUpdate_Resolve;
ComputePSO _cptBlockVolUpdate_Pass3[kBuf][kTG];
ComputePSO _cptBlockQUpdate_Prepare;
ComputePSO _cptBlockQUpdate_Pass1;
ComputePSO _cptBlockQUpdate_Pass2;
ComputePSO _cptBlockQUpdate_Resolve;
// PSO for occupied queue defragmentation
ComputePSO _cptBlockQDefragment;
// PSOs for rendering
GraphicsPSO _gfxVCamRenderPSO[kBuf][kStruct][kFilter];
GraphicsPSO _gfxSensorRenderPSO[kBuf][kStruct][kFilter];
GraphicsPSO _gfxStepInfoVCamPSO[2]; // index is noInstance on/off
GraphicsPSO _gfxStepInfoSensorPSO[2]; // index is noInstance on/off
GraphicsPSO _gfxStepInfoDebugPSO;
GraphicsPSO _gfxHelperWireframePSO;
GraphicsPSO _gfxDebugFuseBlockPSO;
// PSOs for reseting
ComputePSO _cptFuseBlockVolResetPSO;
ComputePSO _cptFuseBlockVolRefreshPSO;
ComputePSO _cptRenderBlockVolResetPSO;
ComputePSO _cptTSDFBufResetPSO[kBuf];

StructuredBuffer _cubeVB;
ByteAddressBuffer _cubeTriangleStripIB;
ByteAddressBuffer _cubeLineStripIB;

inline bool _IsResolutionChanged(const uint3& a, const uint3& b)
{
    return a.x != b.x || a.y != b.y || a.z != b.z;
}

inline HRESULT _Compile(LPCWSTR shaderName,
    const D3D_SHADER_MACRO* macro, ID3DBlob** bolb)
{
    UINT compilerFlag = D3DCOMPILE_OPTIMIZATION_LEVEL3;
#if defined(DEBUG) || defined(_DEBUG)
    compilerFlag = D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_DEBUG;
#endif
    int iLen = (int)wcslen(shaderName);
    char target[8];
    wchar_t fileName[128];
    swprintf_s(fileName, 128, L"TSDFVolume_%ls.hlsl", shaderName);
    switch (shaderName[iLen - 2])
    {
    case 'c': sprintf_s(target, 8, "cs_5_1"); break;
    case 'p': sprintf_s(target, 8, "ps_5_1"); break;
    case 'v': sprintf_s(target, 8, "vs_5_1"); break;
    default:
        PRINTERROR(L"Shader name: %s is Invalid!", shaderName);
    }
    return Graphics::CompileShaderFromFile(Core::GetAssetFullPath(
        fileName).c_str(), macro, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main",
        target, compilerFlag, 0, bolb);
}

void _CreatePSOs()
{
    HRESULT hr;
    // Compile Shaders
    ComPtr<ID3DBlob> quadVCamVS, quadSensorVS;
    ComPtr<ID3DBlob> raycastVCamPS[kBuf][kStruct][kFilter];
    ComPtr<ID3DBlob> raycastSensorPS[kBuf][kStruct][kFilter];
    ComPtr<ID3DBlob> volUpdateCS[kBuf][kStruct];
    ComPtr<ID3DBlob> blockUpdateCS[kBuf][kTG];
    ComPtr<ID3DBlob> tsdfVolResetCS[kBuf];
    ComPtr<ID3DBlob> fuseBlockVolResetCS;
    ComPtr<ID3DBlob> renderBlockVolResetCS;

    ComPtr<ID3DBlob> blockQCreate_Pass1CS;
    ComPtr<ID3DBlob> blockQCreate_Pass2CS;
    ComPtr<ID3DBlob> blockQCreate_ResolveCS;
    ComPtr<ID3DBlob> blockQUpdate_PrepareCS;
    ComPtr<ID3DBlob> blockQUpdate_Pass1CS;
    ComPtr<ID3DBlob> blockQUpdate_Pass2CS;
    ComPtr<ID3DBlob> blockQUpdate_ResolveCS;
    ComPtr<ID3DBlob> blockQDefragmentCS;

    D3D_SHADER_MACRO macro_[] = {
        { "__hlsl", "1" },//0
        { "PREPARE", "0" },//1
        { "PASS_1", "0" },//2
        { "PASS_2", "0" },//3
        { "RESOLVE", "0" },//4
        { nullptr, nullptr }
    };

    macro_[2].Definition = "1";
    V(_Compile(L"BlockQueueCreate_cs", macro_, &blockQCreate_Pass1CS));
    macro_[2].Definition = "0"; macro_[3].Definition = "1";
    V(_Compile(L"BlockQueueCreate_cs", macro_, &blockQCreate_Pass2CS));
    macro_[3].Definition = "0";
    V(_Compile(L"BlockQueueResolve_cs", macro_, &blockQCreate_ResolveCS));
    macro_[1].Definition = "1";
    V(_Compile(L"OccupiedQueueUpdate_cs", macro_, &blockQUpdate_PrepareCS));
    macro_[1].Definition = "0"; macro_[2].Definition = "1";
    V(_Compile(L"OccupiedQueueUpdate_cs", macro_, &blockQUpdate_Pass1CS));
    macro_[2].Definition = "0"; macro_[3].Definition = "1";
    V(_Compile(L"OccupiedQueueUpdate_cs", macro_, &blockQUpdate_Pass2CS));
    macro_[3].Definition = "0"; macro_[4].Definition = "1";
    V(_Compile(L"OccupiedQueueUpdate_cs", macro_, &blockQUpdate_ResolveCS));
    macro_[4].Definition = "0";
    V(_Compile(L"QueueDefragment_cs", macro_, &blockQDefragmentCS));

    D3D_SHADER_MACRO macro[] = {
        { "__hlsl", "1" },//0
        { "TYPED_UAV", "0" },//1
        { "TEX3D_UAV", "0" },//2
        { "FILTER_READ", "0" },//3
        { "ENABLE_BRICKS", "0" },//4
        { "NO_TYPED_LOAD", "0" },//5
        { "TSDFVOL_RESET", "0" },//6
        { "FUSEBLOCKVOL_RESET", "0" },//7
        { "FOR_SENSOR", "0" },//8
        { "FOR_VCAMERA", "0" },//9
        { "THREAD_DIM", "8" },//10
        { "RENDERBLOCKVOL_RESET", "0"},//11
        { nullptr, nullptr }
    };

    if (!_typedLoadSupported) {
        macro[5].Definition = "1";
    }
    macro[8].Definition = "1";
    V(_Compile(L"RayCast_vs", macro, &quadSensorVS));
    macro[8].Definition = "0"; macro[9].Definition = "1";
    V(_Compile(L"RayCast_vs", macro, &quadVCamVS));
    macro[9].Definition = "0"; macro[7].Definition = "1";
    V(_Compile(L"VolumeReset_cs", macro, &fuseBlockVolResetCS));
    macro[7].Definition = "0"; macro[11].Definition = "1";
    V(_Compile(L"VolumeReset_cs", macro, &renderBlockVolResetCS));
    macro[11].Definition = "0";
    uint DefIdx;
    bool compiledOnce = false;
    for (int j = 0; j < kStruct; ++j) {
        macro[4].Definition = j == TSDFVolume::kVoxel ? "0" : "1";
        for (int i = 0; i < kBuf; ++i) {
            switch ((ManagedBuf::Type)i) {
            case ManagedBuf::kTypedBuffer: DefIdx = 1; break;
            case ManagedBuf::k3DTexBuffer: DefIdx = 2; break;
            }
            macro[DefIdx].Definition = "1";
            if (!compiledOnce) {
                macro[6].Definition = "1";
                V(_Compile(L"VolumeReset_cs", macro, &tsdfVolResetCS[i]));
                macro[6].Definition = "0";
                V(_Compile(L"BlocksUpdate_cs", macro,
                    &blockUpdateCS[i][TSDFVolume::k512]));
                macro[10].Definition = "4";
                V(_Compile(L"BlocksUpdate_cs", macro,
                    &blockUpdateCS[i][TSDFVolume::k64]));
                macro[10].Definition = "8";
            }
            V(_Compile(L"VolumeUpdate_cs", macro, &volUpdateCS[i][j]));
            macro[9].Definition = "1";
            V(_Compile(L"RayCast_ps",
                macro, &raycastVCamPS[i][j][TSDFVolume::kNoFilter]));
            macro[3].Definition = "1"; // FILTER_READ
            V(_Compile(L"RayCast_ps",
                macro, &raycastVCamPS[i][j][TSDFVolume::kLinearFilter]));
            macro[3].Definition = "2"; // FILTER_READ
            V(_Compile(L"RayCast_ps",
                macro, &raycastVCamPS[i][j][TSDFVolume::kSamplerLinear]));
            macro[3].Definition = "3"; // FILTER_READ
            V(_Compile(L"RayCast_ps",
                macro, &raycastVCamPS[i][j][TSDFVolume::kSamplerAniso]));
            macro[3].Definition = "0"; // FILTER_READ
            macro[9].Definition = "0"; macro[8].Definition = "1";
            V(_Compile(L"RayCast_ps",
                macro, &raycastSensorPS[i][j][TSDFVolume::kNoFilter]));
            macro[3].Definition = "1"; // FILTER_READ
            V(_Compile(L"RayCast_ps",
                macro, &raycastSensorPS[i][j][TSDFVolume::kLinearFilter]));
            macro[3].Definition = "2"; // FILTER_READ
            V(_Compile(L"RayCast_ps",
                macro, &raycastSensorPS[i][j][TSDFVolume::kSamplerLinear]));
            macro[3].Definition = "3"; // FILTER_READ
            V(_Compile(L"RayCast_ps",
                macro, &raycastSensorPS[i][j][TSDFVolume::kSamplerAniso]));
            macro[3].Definition = "0"; // FILTER_READ
            macro[8].Definition = "0";
            macro[DefIdx].Definition = "0";
        }
        compiledOnce = true;
    }
    // Create Rootsignature
    _rootsig.Reset(4, 1);
    _rootsig.InitStaticSampler(0, Graphics::g_SamplerLinearClampDesc);
    _rootsig[0].InitAsConstantBuffer(0);
    _rootsig[1].InitAsConstantBuffer(1);
    _rootsig[2].InitAsDescriptorRange(
        D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0, numUAVs);
    _rootsig[3].InitAsDescriptorRange(
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, numSRVs);
    _rootsig.Finalize(L"TSDFVolume",
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS);

    // Define the vertex input layout.
    D3D12_INPUT_ELEMENT_DESC inputElementDescs[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,
        D3D12_APPEND_ALIGNED_ELEMENT,
        D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}
    };

    // Create PSO for volume update and volume render
    DXGI_FORMAT ColorBufFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
    DXGI_FORMAT DepthBufFormat = Graphics::g_SceneDepthBuffer.GetFormat();
    DXGI_FORMAT DepthMapFormat = DXGI_FORMAT_R16_UNORM;
    //DXGI_FORMAT ExtractTexFormat = DXGI_FORMAT_R16_UINT;

#define CreatePSO( ObjName, Shader)\
ObjName.SetRootSignature(_rootsig);\
ObjName.SetComputeShader(Shader->GetBufferPointer(), Shader->GetBufferSize());\
ObjName.Finalize();

    CreatePSO(_cptFuseBlockVolResetPSO, fuseBlockVolResetCS);
    CreatePSO(_cptRenderBlockVolResetPSO, renderBlockVolResetCS);
    CreatePSO(_cptBlockVolUpdate_Pass1, blockQCreate_Pass1CS);
    CreatePSO(_cptBlockVolUpdate_Pass2, blockQCreate_Pass2CS);
    CreatePSO(_cptBlockVolUpdate_Resolve, blockQCreate_ResolveCS);
    CreatePSO(_cptBlockQUpdate_Prepare, blockQUpdate_PrepareCS);
    CreatePSO(_cptBlockQUpdate_Pass1, blockQUpdate_Pass1CS);
    CreatePSO(_cptBlockQUpdate_Pass2, blockQUpdate_Pass2CS);
    CreatePSO(_cptBlockQUpdate_Resolve, blockQUpdate_ResolveCS);
    CreatePSO(_cptBlockQDefragment, blockQDefragmentCS);

    compiledOnce = false;
    for (int k = 0; k < kStruct; ++k) {
        for (int i = 0; i < kBuf; ++i) {
            for (int j = 0; j < kFilter; ++j) {
                _gfxVCamRenderPSO[i][k][j].SetRootSignature(_rootsig);
                _gfxVCamRenderPSO[i][k][j].SetInputLayout(0, nullptr);
                _gfxVCamRenderPSO[i][k][j].SetRasterizerState(
                    Graphics::g_RasterizerDefault);
                _gfxVCamRenderPSO[i][k][j].SetBlendState(
                    Graphics::g_BlendDisable);
                _gfxVCamRenderPSO[i][k][j].SetDepthStencilState(
                    Graphics::g_DepthStateReadWrite);
                _gfxVCamRenderPSO[i][k][j].SetSampleMask(UINT_MAX);
                _gfxVCamRenderPSO[i][k][j].SetPrimitiveTopologyType(
                    D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
                _gfxVCamRenderPSO[i][k][j].SetRenderTargetFormats(
                    1, &DepthMapFormat, DepthBufFormat);
                _gfxVCamRenderPSO[i][k][j].SetPixelShader(
                    raycastVCamPS[i][k][j]->GetBufferPointer(),
                    raycastVCamPS[i][k][j]->GetBufferSize());
                _gfxSensorRenderPSO[i][k][j] = _gfxVCamRenderPSO[i][k][j];
                _gfxSensorRenderPSO[i][k][j].SetDepthStencilState(
                    Graphics::g_DepthStateDisabled);
                _gfxSensorRenderPSO[i][k][j].SetVertexShader(
                    quadSensorVS->GetBufferPointer(),
                    quadSensorVS->GetBufferSize());
                _gfxSensorRenderPSO[i][k][j].SetRenderTargetFormats(
                    1, &DepthMapFormat, DXGI_FORMAT_UNKNOWN);
                _gfxSensorRenderPSO[i][k][j].SetPixelShader(
                    raycastSensorPS[i][k][j]->GetBufferPointer(),
                    raycastSensorPS[i][k][j]->GetBufferSize());
                _gfxSensorRenderPSO[i][k][j].Finalize();
                _gfxVCamRenderPSO[i][k][j].SetVertexShader(
                    quadVCamVS->GetBufferPointer(),
                    quadVCamVS->GetBufferSize());
                _gfxVCamRenderPSO[i][k][j].Finalize();
            }
            CreatePSO(_cptUpdatePSO[i][k], volUpdateCS[i][k]);
            if (!compiledOnce) {
                CreatePSO(_cptTSDFBufResetPSO[i], tsdfVolResetCS[i]);
                CreatePSO(_cptBlockVolUpdate_Pass3[i][TSDFVolume::k512],
                    blockUpdateCS[i][TSDFVolume::k512]);
                CreatePSO(_cptBlockVolUpdate_Pass3[i][TSDFVolume::k64],
                    blockUpdateCS[i][TSDFVolume::k64]);
            }
        }
        compiledOnce = true;
    }
#undef  CreatePSO

    // Create PSO for render near far plane
    ComPtr<ID3DBlob> stepInfoPS, stepInfoDebugPS, wireframePS;
    ComPtr<ID3DBlob> stepInfoVCamVS[2], stepInfoSensorVS[2], wireframeVS;
    ComPtr<ID3DBlob> debugFuseBlockVS;
    ComPtr<ID3DBlob> debugFuseBlockPS;
    D3D_SHADER_MACRO macro1[] = {
        {"__hlsl", "1"},
        {"DEBUG_VIEW", "0"},
        {"FOR_VCAMERA", "1"},
        {"FOR_SENSOR", "0"},
        {"FUSEDEBUG", "0"},
        {nullptr, nullptr}
    };
    V(_Compile(L"HelperWireframe_vs", macro1, &wireframeVS));
    V(_Compile(L"HelperWireframe_ps", macro1, &wireframePS));
    V(_Compile(L"StepInfo_ps", macro1, &stepInfoPS));
    V(_Compile(L"StepInfo_vs", macro1, &stepInfoVCamVS[0]));
    V(_Compile(L"StepInfoNoInstance_vs", macro1, &stepInfoVCamVS[1]));
    macro1[2].Definition = "0"; macro1[3].Definition = "1";
    V(_Compile(L"StepInfo_vs", macro1, &stepInfoSensorVS[0]));
    V(_Compile(L"StepInfoNoInstance_vs", macro1, &stepInfoSensorVS[1]));
    macro1[1].Definition = "1";
    V(_Compile(L"StepInfo_ps", macro1, &stepInfoDebugPS));
    macro1[4].Definition = "1";
    V(_Compile(L"StepInfo_vs", macro1, &debugFuseBlockVS));
    V(_Compile(L"StepInfo_ps", macro1, &debugFuseBlockPS));

    _gfxStepInfoVCamPSO[0].SetRootSignature(_rootsig);
    _gfxStepInfoVCamPSO[0].SetPrimitiveRestart(
        D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF);
    _gfxStepInfoVCamPSO[0].SetInputLayout(
        _countof(inputElementDescs), inputElementDescs);
    _gfxStepInfoVCamPSO[0].SetDepthStencilState(
        Graphics::g_DepthStateDisabled);
    _gfxStepInfoVCamPSO[0].SetSampleMask(UINT_MAX);
    _gfxStepInfoVCamPSO[0].SetVertexShader(
        stepInfoVCamVS[0]->GetBufferPointer(),
        stepInfoVCamVS[0]->GetBufferSize());
    _gfxStepInfoDebugPSO = _gfxStepInfoVCamPSO[0];
    _gfxStepInfoDebugPSO.SetDepthStencilState(
        Graphics::g_DepthStateReadOnly);
    _gfxStepInfoVCamPSO[0].SetRasterizerState(
        Graphics::g_RasterizerTwoSided);
    D3D12_RASTERIZER_DESC rastDesc = Graphics::g_RasterizerTwoSided;
    rastDesc.FillMode = D3D12_FILL_MODE_WIREFRAME;
    _gfxStepInfoDebugPSO.SetRasterizerState(rastDesc);
    _gfxStepInfoDebugPSO.SetBlendState(Graphics::g_BlendDisable);
    D3D12_BLEND_DESC blendDesc = {};
    blendDesc.AlphaToCoverageEnable = false;
    blendDesc.IndependentBlendEnable = false;
    blendDesc.RenderTarget[0].BlendEnable = true;
    blendDesc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_MIN;
    blendDesc.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_MAX;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
    blendDesc.RenderTarget[0].RenderTargetWriteMask =
        D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN;
    _gfxStepInfoVCamPSO[0].SetBlendState(blendDesc);
    _gfxStepInfoVCamPSO[0].SetPrimitiveTopologyType(
        D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    _gfxStepInfoDebugPSO.SetPrimitiveTopologyType(
        D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE);
    _gfxStepInfoDebugPSO.SetRenderTargetFormats(
        1, &ColorBufFormat, DepthBufFormat);
    _gfxHelperWireframePSO = _gfxStepInfoDebugPSO;
    _gfxHelperWireframePSO.SetVertexShader(
        wireframeVS->GetBufferPointer(), wireframeVS->GetBufferSize());
    _gfxHelperWireframePSO.SetPixelShader(
        wireframePS->GetBufferPointer(), wireframePS->GetBufferSize());
    _gfxHelperWireframePSO.Finalize();
    _gfxStepInfoVCamPSO[0].SetRenderTargetFormats(
        1, &_stepInfoTexFormat, DXGI_FORMAT_UNKNOWN);
    _gfxStepInfoVCamPSO[0].SetPixelShader(
        stepInfoPS->GetBufferPointer(), stepInfoPS->GetBufferSize());
    _gfxStepInfoDebugPSO.SetPixelShader(
        stepInfoDebugPS->GetBufferPointer(),
        stepInfoDebugPS->GetBufferSize());
    _gfxStepInfoSensorPSO[0] = _gfxStepInfoVCamPSO[0];
    _gfxStepInfoSensorPSO[1] = _gfxStepInfoSensorPSO[0];
    _gfxStepInfoSensorPSO[1].SetInputLayout(0, nullptr);
    _gfxStepInfoSensorPSO[0].SetVertexShader(
        stepInfoSensorVS[0]->GetBufferPointer(),
        stepInfoSensorVS[0]->GetBufferSize());
    _gfxStepInfoSensorPSO[1].SetVertexShader(
        stepInfoSensorVS[1]->GetBufferPointer(),
        stepInfoSensorVS[1]->GetBufferSize());
    _gfxStepInfoSensorPSO[0].Finalize();
    _gfxStepInfoSensorPSO[1].Finalize();
    _gfxStepInfoVCamPSO[1] = _gfxStepInfoVCamPSO[0];
    _gfxStepInfoVCamPSO[1].SetInputLayout(0, nullptr);
    _gfxStepInfoVCamPSO[1].SetVertexShader(
        stepInfoVCamVS[1]->GetBufferPointer(),
        stepInfoVCamVS[1]->GetBufferSize());
    _gfxStepInfoVCamPSO[0].Finalize();
    _gfxStepInfoVCamPSO[1].Finalize();
    _gfxDebugFuseBlockPSO = _gfxStepInfoDebugPSO;
    _gfxDebugFuseBlockPSO.SetVertexShader(
        debugFuseBlockVS->GetBufferPointer(),
        debugFuseBlockVS->GetBufferSize());
    _gfxDebugFuseBlockPSO.SetPixelShader(
        debugFuseBlockPS->GetBufferPointer(),
        debugFuseBlockPS->GetBufferSize());
    _gfxDebugFuseBlockPSO.Finalize();
    _gfxStepInfoDebugPSO.Finalize();
}

void _CreateResource()
{
    HRESULT hr;
    // Feature support checking
    D3D12_FEATURE_DATA_D3D12_OPTIONS FeatureData = {};
    V(Graphics::g_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS,
        &FeatureData, sizeof(FeatureData)));
    if (SUCCEEDED(hr)) {
        if (FeatureData.TypedUAVLoadAdditionalFormats) {
            D3D12_FEATURE_DATA_FORMAT_SUPPORT FormatSupport =
            {DXGI_FORMAT_R8_SNORM, D3D12_FORMAT_SUPPORT1_NONE,
                D3D12_FORMAT_SUPPORT2_NONE};
            V(Graphics::g_device->CheckFeatureSupport(
                D3D12_FEATURE_FORMAT_SUPPORT, &FormatSupport,
                sizeof(FormatSupport)));
            if (FAILED(hr)) {
                PRINTERROR("Checking Feature Support Failed");
            }
            if ((FormatSupport.Support2 &
                D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) != 0) {
                _typedLoadSupported = true;
                PRINTINFO("Typed load is supported");
            } else {
                PRINTWARN("Typed load is not supported");
            }
        } else {
            PRINTWARN("TypedLoad AdditionalFormats load is not supported");
        }
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC UAVDesc = {};
    UAVDesc.Format = DXGI_FORMAT_R8_UNORM;
    UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

    D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc = {};
    SRVDesc.Format = DXGI_FORMAT_R8_UNORM;
    SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    SRVDesc.Texture2D.MipLevels = 1;

    for (int i = 0; i < numUAVs; ++i) {
        _nullUAVs[i] = Graphics::g_pCSUDescriptorHeap->Append().GetCPUHandle();
        Graphics::g_device->CreateUnorderedAccessView(
            NULL, NULL, &UAVDesc, _nullUAVs[i]);
    }
    for (int i = 0; i < numSRVs; ++i) {
        _nullSRVs[i] = Graphics::g_pCSUDescriptorHeap->Append().GetCPUHandle();
        Graphics::g_device->CreateShaderResourceView(
            NULL, &SRVDesc, _nullSRVs[i]);
    }

    _CreatePSOs();

    const uint32_t vertexBufferSize = sizeof(cubeVertices);
    _cubeVB.Create(L"Vertex Buffer", ARRAYSIZE(cubeVertices),
        sizeof(XMFLOAT3), (void*)cubeVertices);

    _cubeTriangleStripIB.Create(L"Cube TriangleStrip Index Buffer",
        ARRAYSIZE(cubeTrianglesStripIndices), sizeof(uint16_t),
        (void*)cubeTrianglesStripIndices);

    _cubeLineStripIB.Create(L"Cube LineStrip Index Buffer",
        ARRAYSIZE(cubeLineStripIndices), sizeof(uint16_t),
        (void*)cubeLineStripIndices);
}
}

TSDFVolume::TSDFVolume()
    : _volBuf(XMUINT3(64, 64, 64)),
    _nearFarForVisual(XMVectorSet(MAX_DEPTH, 0, 0, 0)),
    _nearFarForProcess(XMVectorSet(MAX_DEPTH, 0, 0, 0))
{
    _volParam = &_cbPerCall.vParam;
    _volParam->fMaxWeight = 1.f;
    _volParam->fVoxelSize = 10.f / 128.f;
    _cbPerCall.f2DepthRange = float2(-0.2f, -12.f);
    _cbPerCall.iDefragmentThreshold = 200000;
    // Create helper wireframe for frustum
    float tanX = (DEPTH_RESO.x * 0.5f) / DEPTH_F.x;
    float tanY = (DEPTH_RESO.y * 0.5f) / DEPTH_F.y;
    _cbPerCall.mXForm[1].r[0] = {2.f * tanX, 0.f, 0.f, 0.f};
    _cbPerCall.mXForm[1].r[1] = {0.f, 2.f * tanY, 0.f, 0.f};
    _cbPerCall.mXForm[1].r[2] = {0.f, 0.f, 1.f, 0.4f};
    _cbPerCall.mXForm[1].r[3] = {0.f, 0.f, -0.5f, -0.2f};
    _cbPerCall.mXForm[2] = _cbPerCall.mXForm[1];
    _cbPerCall.mXForm[2].r[2] = {0.f, 0.f, 1.f, 5.f};
    _cbPerCall.mXForm[2].r[3] = {0.f, 0.f, -0.5f, -2.5f};

    if (_fuseBlockVoxelRatio == 4) _TGSize = k64;
    if (_fuseBlockVoxelRatio == 8) _TGSize = k512;
}

TSDFVolume::~TSDFVolume()
{
}

void
TSDFVolume::CreateResource(
    const uint2& depthReso, LinearAllocator& uploadHeapAlloc)
{
    ASSERT(Graphics::g_device);
    // Create resource for volume
    _volBuf.CreateResource();

    // Create resource for job count for OccupiedBlockUpdate pass
    _jobParamBuf.Create(L"JobCountBuf", 5, 4);

    // Create debug buffer for all queue buffer size. 
    // 0 : _occupiedBlocksBuf.counter
    // 1 : _updateBlocksBuf.counter
    // 2 : _newOccupiedBlocksBuf.counter
    // 3 : _freedOccupiedBlocksBuf.counter
    _debugBuf.Create(L"DebugBuf", 8, 4);

    // Initial value for dispatch indirect args. args are thread group count
    // x, y, z. Since we only need 1 dimensional dispatch thread group, we
    // pre-populate 1, 1 for threadgroup Ys and Zs
    __declspec(align(16)) const uint32_t initArgs[15] = {
        0, 1, 1, // for VolumeUpdate_Pass2, X = OccupiedBlocks / WARP_SIZE
        0, 1, 1, // for VolumeUpdate_Pass3, X = numUpdateBlock * TG/Block
        0, 1, 1, // for OccupiedBlockUpdate_Pass2, X = numFreeSlots / WARP_SIZE
        0, 1, 1, // for OccupiedBlockUpdate_Pass3, X = numLeftOver / WARP_SIZE
        0, 1, 1, // for defragment pass, X = numLeftOver / WARP_SIZE
    };
    _indirectParams.Create(L"TSDFVolume Indirect Params",
        1, 5 * sizeof(D3D12_DISPATCH_ARGUMENTS), initArgs);

    std::call_once(_psoCompiled_flag, _CreateResource);

    _cbPerCall.i2DepthReso = int2(depthReso.x, depthReso.y);

    _gpuCB.Create(L"TSDFVolume_CB", 1, sizeof(PerCallDataCB),
        (void*)&_cbPerCall);
    _pUploadCB = new DynAlloc(
        std::move(uploadHeapAlloc.Allocate(sizeof(PerCallDataCB))));

    _depthViewPort.Width = static_cast<float>(depthReso.x);
    _depthViewPort.Height = static_cast<float>(depthReso.y);
    _depthViewPort.MaxDepth = 1.f;

    _depthSissorRect.right = static_cast<LONG>(depthReso.x);
    _depthSissorRect.bottom = static_cast<LONG>(depthReso.y);

    _nearFarForProcess.Create(L"StepInfoTex_Proc", depthReso.x, depthReso.y, 0,
        _stepInfoTexFormat);

    const uint3 reso = _volBuf.GetReso();
    _submittedReso = reso;
    _UpdateVolumeSettings(reso, _volParam->fVoxelSize);
    _UpdateBlockSettings(_fuseBlockVoxelRatio, _renderBlockVoxelRatio);

    // Create Spacial Structure Buffer
    Graphics::g_cmdListMngr.IdleGPU();
    _CreateRenderBlockVol(reso, _renderBlockVoxelRatio);
    _CreateFuseBlockVolAndRelatedBuf(reso, _fuseBlockVoxelRatio);
}

void
TSDFVolume::Destory()
{
    _debugBuf.Destroy();
    _jobParamBuf.Destroy();
    _indirectParams.Destroy();
    _updateBlocksBuf.Destroy();
    _occupiedBlocksBuf.Destroy();
    _newFuseBlocksBuf.Destroy();
    _freedFuseBlocksBuf.Destroy();
    _volBuf.Destroy();
    _fuseBlockVol.Destroy();
    _renderBlockVol.Destroy();
    _nearFarForVisual.Destroy();
    _nearFarForProcess.Destroy();
    _cubeVB.Destroy();
    _cubeTriangleStripIB.Destroy();
    _cubeLineStripIB.Destroy();
}

void
TSDFVolume::ResizeVisualSurface(uint32_t width, uint32_t height)
{
    _cbStaled = true;
    float fAspectRatio = width / (FLOAT)height;
    _cbPerCall.fWideHeightRatio = fAspectRatio;
    _cbPerCall.fClipDist = 0.1f;
    float fHFov = XM_PIDIV4;
    _cbPerCall.fTanHFov = tan(fHFov / 2.f);

    _depthVisViewPort.Width = static_cast<float>(width);
    _depthVisViewPort.Height = static_cast<float>(height);
    _depthVisViewPort.MaxDepth = 1.f;

    _depthVisSissorRect.right = static_cast<LONG>(width);
    _depthVisSissorRect.bottom = static_cast<LONG>(height);

    _nearFarForVisual.Destroy();
    _nearFarForVisual.Create(L"StepInfoTex_Visual", width, height, 1,
        _stepInfoTexFormat);
}

void
TSDFVolume::ResetAllResource()
{
    ComputeContext& cptCtx = ComputeContext::Begin(L"ResetContext");
    cptCtx.SetRootSignature(_rootsig);
    cptCtx.SetConstantBuffer(1, _gpuCB.RootConstantBufferView());
    Trans(cptCtx, _occupiedBlocksBuf, UAV);
    Trans(cptCtx, _newFuseBlocksBuf, UAV);
    Trans(cptCtx, _freedFuseBlocksBuf, UAV);
    _RefreshFuseBlockVol(cptCtx);
    _CleanTSDFVols(cptCtx, _curBufInterface);
    _CleanFuseBlockVol(cptCtx);
    _CleanRenderBlockVol(cptCtx);
    _ClearBlockQueues(cptCtx);
    cptCtx.Finish(true);
}

void
TSDFVolume::PreProcessing(const DirectX::XMMATRIX& mVCamProj_T,
    const DirectX::XMMATRIX& mVCamView_T,
    const DirectX::XMMATRIX& mSensor_T)
{
    _UpdateRenderCamData(mVCamProj_T, mVCamView_T);
    _UpdateSensorData(mSensor_T);
    ManagedBuf::BufInterface newBufInterface = _volBuf.GetResource();
    _curBufInterface = newBufInterface;

    const uint3& reso = _volBuf.GetReso();
    if (_IsResolutionChanged(reso, _curReso)) {
        _curReso = reso;
        _UpdateVolumeSettings(reso, _volParam->fVoxelSize);
        _UpdateBlockSettings(_fuseBlockVoxelRatio,
            _renderBlockVoxelRatio);
        Graphics::g_cmdListMngr.IdleGPU();
        _CreateRenderBlockVol(
            _volBuf.GetReso(), _renderBlockVoxelRatio);
        _CreateFuseBlockVolAndRelatedBuf(reso, _fuseBlockVoxelRatio);
    }
}

void
TSDFVolume::DefragmentActiveBlockQueue(ComputeContext& cptCtx)
{
    Trans(cptCtx, _occupiedBlocksBuf, UAV);
    Trans(cptCtx, _freedFuseBlocksBuf, csSRV);
    Trans(cptCtx, _jobParamBuf, csSRV);
    Trans(cptCtx, _indirectParams, IARG);
    {
        GPU_PROFILE(cptCtx, L"Defragment");
        cptCtx.SetPipelineState(_cptBlockQDefragment);
        cptCtx.SetRootSignature(_rootsig);
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numUAVs> UAVs = _nullUAVs;
        UAVs[0] = _occupiedBlocksBuf.GetUAV();
        UAVs[1] = _freedFuseBlocksBuf.GetCounterUAV(cptCtx);
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numSRVs> SRVs = _nullSRVs;
        SRVs[0] = _freedFuseBlocksBuf.GetSRV();
        SRVs[1] = _jobParamBuf.GetSRV();
        Bind(cptCtx, 2, 0, numUAVs, UAVs.data());
        Bind(cptCtx, 3, 0, numSRVs, SRVs.data());
        cptCtx.DispatchIndirect(_indirectParams, 48);
    }
    //BeginTrans(cptCtx, _occupiedBlocksBuf, csSRV);
    BeginTrans(cptCtx, _freedFuseBlocksBuf, UAV);
    BeginTrans(cptCtx, _jobParamBuf, UAV);
}

void
TSDFVolume::UpdateVolume(ComputeContext& cptCtx, ColorBuffer* pDepthTex,
    ColorBuffer* pWeightTex)
{
    cptCtx.SetRootSignature(_rootsig);
    _UpdateAndBindConstantBuffer(cptCtx);
    BeginTrans(cptCtx, _renderBlockVol, UAV);
    Trans(cptCtx, *_curBufInterface.resource[0], UAV);
    Trans(cptCtx, *_curBufInterface.resource[1], UAV);
    _UpdateVolume(cptCtx, _curBufInterface, pDepthTex, pWeightTex);
    BeginTrans(cptCtx, *_curBufInterface.resource[0], psSRV);
    BeginTrans(cptCtx, *_curBufInterface.resource[1], UAV);
    if (_useStepInfoTex) {
        BeginTrans(cptCtx, _renderBlockVol, vsSRV | psSRV);
    }
}

void
TSDFVolume::ExtractSurface(GraphicsContext& gfxCtx, ColorBuffer* pDepthOut,
    ColorBuffer* pVisDepthOut, DepthBuffer* pVisDepthBuf)
{
    if (pDepthOut) Trans(gfxCtx, *pDepthOut, RTV);
    if (pVisDepthOut) Trans(gfxCtx, *pVisDepthOut, RTV);
    if (pVisDepthOut && pVisDepthBuf) Trans(gfxCtx, *pVisDepthBuf, DSV);
    if (_useStepInfoTex) {
        if (pDepthOut) Trans(gfxCtx, _nearFarForProcess, RTV);
        if (pVisDepthOut) Trans(gfxCtx, _nearFarForVisual, RTV);
        gfxCtx.FlushResourceBarriers();
    } else {
        gfxCtx.FlushResourceBarriers();
    }

    gfxCtx.SetRootSignature(_rootsig);
    _UpdateAndBindConstantBuffer(gfxCtx);
    gfxCtx.SetVertexBuffer(0, _cubeVB.VertexBufferView());

    Trans(gfxCtx, _renderBlockVol, vsSRV | psSRV);
    if (pDepthOut && _useStepInfoTex) {
        gfxCtx.SetViewport(_depthViewPort);
        gfxCtx.SetScisor(_depthSissorRect);
        GPU_PROFILE(gfxCtx, L"NearFar_Proc");
        gfxCtx.ClearColor(_nearFarForProcess);
        gfxCtx.SetRenderTarget(_nearFarForProcess.GetRTV());
        _RenderNearFar(gfxCtx, true);
        // Early submit to keep GPU busy
        gfxCtx.Flush();
        gfxCtx.SetRootSignature(_rootsig);
        _UpdateAndBindConstantBuffer(gfxCtx);
        gfxCtx.SetVertexBuffer(0, _cubeVB.VertexBufferView());
    }
    if (pVisDepthOut && _useStepInfoTex) {
        gfxCtx.SetViewport(_depthVisViewPort);
        gfxCtx.SetScisor(_depthVisSissorRect);
        BeginTrans(gfxCtx, _nearFarForProcess, psSRV);
        GPU_PROFILE(gfxCtx, L"NearFar_Visu");
        gfxCtx.ClearColor(_nearFarForVisual);
        gfxCtx.SetRenderTarget(_nearFarForVisual.GetRTV());
        _RenderNearFar(gfxCtx);
    }
    if (pDepthOut) {
        if (_useStepInfoTex) {
            BeginTrans(gfxCtx, _nearFarForVisual, psSRV);
            Trans(gfxCtx, _nearFarForProcess, psSRV);
        }
        Trans(gfxCtx, *_curBufInterface.resource[0], psSRV);
        {
            GPU_PROFILE(gfxCtx, L"Volume_Proc");
            gfxCtx.ClearColor(*pDepthOut);
            gfxCtx.SetViewport(_depthViewPort);
            gfxCtx.SetScisor(_depthSissorRect);
            gfxCtx.SetRenderTarget(pDepthOut->GetRTV());
            _RenderVolume(gfxCtx, _curBufInterface, true);
        }
        if (_useStepInfoTex) {
            BeginTrans(gfxCtx, _nearFarForProcess, RTV);
        }
    }
    if (pVisDepthOut) {
        if (_useStepInfoTex) {
            Trans(gfxCtx, _nearFarForVisual, psSRV);
        }
        Trans(gfxCtx, *_curBufInterface.resource[0], psSRV);
        {
            GPU_PROFILE(gfxCtx, L"Volume_Visu");
            gfxCtx.ClearColor(*pVisDepthOut);
            if (pVisDepthBuf) gfxCtx.ClearDepth(*pVisDepthBuf);
            gfxCtx.SetViewport(_depthVisViewPort);
            gfxCtx.SetScisor(_depthVisSissorRect);
            if (pVisDepthBuf) {
                gfxCtx.SetRenderTargets(
                    1, &pVisDepthOut->GetRTV(), pVisDepthBuf->GetDSV());
            } else {
                gfxCtx.SetRenderTarget(pVisDepthOut->GetRTV());
            }
            _RenderVolume(gfxCtx, _curBufInterface);
        }
        if (_useStepInfoTex) {
            BeginTrans(gfxCtx, _nearFarForVisual, RTV);
        }
    }
    BeginTrans(gfxCtx, *_curBufInterface.resource[0], UAV);
}

void
TSDFVolume::RenderDebugGrid(GraphicsContext& gfxCtx,
    ColorBuffer* pColor, DepthBuffer* pDepth)
{
    Trans(gfxCtx, *pColor, RTV);
    Trans(gfxCtx, *pDepth, DSV);

    GPU_PROFILE(gfxCtx, L"DebugGrid_Render");
    gfxCtx.SetRootSignature(_rootsig);
    _UpdateAndBindConstantBuffer(gfxCtx);
    gfxCtx.SetVertexBuffer(0, _cubeVB.VertexBufferView());
    gfxCtx.SetViewport(_depthVisViewPort);
    gfxCtx.SetScisor(_depthVisSissorRect);
    gfxCtx.SetRenderTargets(1, &pColor->GetRTV(), pDepth->GetDSV());
    gfxCtx.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINESTRIP);
    gfxCtx.SetIndexBuffer(_cubeLineStripIB.IndexBufferView());
    _RenderHelperWireframe(gfxCtx);
    if (_blockVolumeUpdate && _stepInfoDebug) {
        _RenderBrickGrid(gfxCtx);
    }
}

void
TSDFVolume::RenderGui()
{
    using namespace ImGui;
    static bool showPenal = true;
    if (!CollapsingHeader("TSDFVolume", 0, true, true)) {
        return;
    }
    if (Button("RecompileShaders##TSDFVolume")) {
        _CreatePSOs();
    }
    SameLine();
    if (Button("ResetVolume##TSDFVolume")) {
        ResetAllResource();
    }
    Separator();
    Checkbox("Block Volume Update", &_blockVolumeUpdate);
    SameLine();
    Checkbox("Debug", &_readBackGPUBufStatus);
    if (_blockVolumeUpdate) {
        _cbStaled |= SliderInt("Defragment Threshold",
            &_cbPerCall.iDefragmentThreshold, 5000, 500000);
    }
    if (_readBackGPUBufStatus && _blockVolumeUpdate) {
        char buf[64];
        float data = (float)_readBackData[0] / _occupiedQSize;
        sprintf_s(buf, 64, "%d/%d OccupiedQ", _readBackData[0], _occupiedQSize);
        ProgressBar(data, ImVec2(-1.f, 0.f), buf);
        data = (float)_readBackData[1] / _updateQSize;
        sprintf_s(buf, 64, "%d/%d UpdateQ", _readBackData[1], _updateQSize);
        ProgressBar(data, ImVec2(-1.f, 0.f), buf);
        data = (float)_readBackData[2] / _newQSize;
        sprintf_s(buf, 64, "%d/%d NewQ", _readBackData[2], _newQSize);
        ProgressBar(data, ImVec2(-1.f, 0.f), buf);
        data = (float)_readBackData[3] / _freedQSize;
        sprintf_s(buf, 64, "%d/%d FreeQ", _readBackData[3], _freedQSize);
        ProgressBar(data, ImVec2(-1.f, 0.f), buf);

        sprintf_s(buf, 64, "UpdateBlock:%d", _readBackData[4]);
        Text(buf);
        sprintf_s(buf, 64, "DispatchCt:%d", _readBackData[5]);
        Text(buf);
        if (_readBackData[4] != _readBackData[5]) {
            PRINTERROR("UpdateBlock:%d DispatchCt:%d", _readBackData[4], _readBackData[5]);
        }
    }
    Separator();
    Checkbox("StepInfoTex", &_useStepInfoTex); SameLine();
    Checkbox("NoInstance", &_noInstance); SameLine();
    Checkbox("DebugGrid", &_stepInfoDebug);
    Separator();
    static int iFilterType = (int)_filterType;
    RadioButton("No Filter", &iFilterType, kNoFilter); SameLine();
    RadioButton("Linear Filter", &iFilterType, kLinearFilter);
    RadioButton("Linear Sampler", &iFilterType, kSamplerLinear); SameLine();
    RadioButton("Aniso Sampler", &iFilterType, kSamplerAniso);
    if (_volBuf.GetType() != ManagedBuf::k3DTexBuffer &&
        iFilterType > kLinearFilter) {
        iFilterType = _filterType;
    }
    _filterType = (FilterType)iFilterType;

    Separator();
    Text("Buffer Settings:");
    static int uBit = _volBuf.GetBit();
    static int uType = _volBuf.GetType();
    RadioButton("8Bit", &uBit, ManagedBuf::k8Bit); SameLine();
    RadioButton("16Bit", &uBit, ManagedBuf::k16Bit); SameLine();
    RadioButton("32Bit", &uBit, ManagedBuf::k32Bit);
    RadioButton("Typed Buffer", &uType, ManagedBuf::kTypedBuffer); SameLine();
    RadioButton("Tex3D Buffer", &uType, ManagedBuf::k3DTexBuffer);
    if (iFilterType > kLinearFilter &&
        uType != ManagedBuf::k3DTexBuffer) {
        uType = _volBuf.GetType();
    }
    if ((uType != _volBuf.GetType() || uBit != _volBuf.GetBit()) &&
        !_volBuf.ChangeResource(_volBuf.GetReso(), (ManagedBuf::Type)uType,
        (ManagedBuf::Bit)uBit)) {
        uType = _volBuf.GetType();
    }

    static bool bNeedUpdate = false;
    Separator();
    Text("RenderBlockRatio:");
    static int renderBlockRatio = (int)_renderBlockVoxelRatio;
    RadioButton("4x4x4##Render", &renderBlockRatio, 4); SameLine();
    RadioButton("8x8x8##Render", &renderBlockRatio, 8); SameLine();
    RadioButton("16x16x16##Render", &renderBlockRatio, 16); SameLine();
    RadioButton("32x32x32##Render", &renderBlockRatio, 32);

    Separator();
    Text("FuseBlockRatio:");
    static int fuseBlockRatio = (int)_fuseBlockVoxelRatio;
    RadioButton("4x4x4##Fuse", &fuseBlockRatio, 4); SameLine();
    RadioButton("8x8x8##Fuse", &fuseBlockRatio, 8);
    Separator();
    if (fuseBlockRatio != _fuseBlockVoxelRatio) {
        if (fuseBlockRatio <= renderBlockRatio) {
            bNeedUpdate = true;
            _fuseBlockVoxelRatio = fuseBlockRatio;
            if (_fuseBlockVoxelRatio == 4) _TGSize = k64;
            if (_fuseBlockVoxelRatio == 8) _TGSize = k512;
        } else {
            fuseBlockRatio = _fuseBlockVoxelRatio;
        }
    }
    if (renderBlockRatio != _renderBlockVoxelRatio) {
        if (renderBlockRatio >= fuseBlockRatio) {
            bNeedUpdate = true;
            _renderBlockVoxelRatio = renderBlockRatio;
        } else {
            renderBlockRatio = _renderBlockVoxelRatio;
        }
    }
    if (bNeedUpdate) {
        bNeedUpdate = false;
        _UpdateBlockSettings(_fuseBlockVoxelRatio, _renderBlockVoxelRatio);
        Graphics::g_cmdListMngr.IdleGPU();
        _CreateRenderBlockVol(_volBuf.GetReso(), _renderBlockVoxelRatio);
        _CreateFuseBlockVolAndRelatedBuf(_curReso, _fuseBlockVoxelRatio);
    }

    Text("Volume Size Settings:");
    static uint3 uiReso = _volBuf.GetReso();
    AlignFirstTextHeightToWidgets();
    Text("X:"); SameLine();
    RadioButton("32##X", (int*)&uiReso.x, 32); SameLine();
    RadioButton("64##X", (int*)&uiReso.x, 64); SameLine();
    RadioButton("128##X", (int*)&uiReso.x, 128); SameLine();
    RadioButton("256##X", (int*)&uiReso.x, 256); SameLine();
    RadioButton("384##X", (int*)&uiReso.x, 384); SameLine();
    RadioButton("512##X", (int*)&uiReso.x, 512);

    AlignFirstTextHeightToWidgets();
    Text("Y:"); SameLine();
    RadioButton("32##Y", (int*)&uiReso.y, 32); SameLine();
    RadioButton("64##Y", (int*)&uiReso.y, 64); SameLine();
    RadioButton("128##Y", (int*)&uiReso.y, 128); SameLine();
    RadioButton("256##Y", (int*)&uiReso.y, 256); SameLine();
    RadioButton("384##Y", (int*)&uiReso.y, 384); SameLine();
    RadioButton("512##Y", (int*)&uiReso.y, 512);

    AlignFirstTextHeightToWidgets();
    Text("Z:"); SameLine();
    RadioButton("32##Z", (int*)&uiReso.z, 32); SameLine();
    RadioButton("64##Z", (int*)&uiReso.z, 64); SameLine();
    RadioButton("128##Z", (int*)&uiReso.z, 128); SameLine();
    RadioButton("256##Z", (int*)&uiReso.z, 256); SameLine();
    RadioButton("384##Z", (int*)&uiReso.z, 384); SameLine();
    RadioButton("512##Z", (int*)&uiReso.z, 512);
    if ((_IsResolutionChanged(uiReso, _submittedReso) ||
        _volBuf.GetType() != uType) &&
        _volBuf.ChangeResource(
            uiReso, _volBuf.GetType(), ManagedBuf::k32Bit)) {
        PRINTINFO("Reso:%dx%dx%d", uiReso.x, uiReso.y, uiReso.z);
        _submittedReso = uiReso;
    } else {
        uiReso = _submittedReso;
    }
    _cbStaled |= SliderFloat("MaxWeight", &_volParam->fMaxWeight, 1.f, 500.f);
    static float fVoxelSize = _volParam->fVoxelSize;
    if (SliderFloat("VoxelSize",
        &fVoxelSize, 1.f / 256.f, 10.f / 128.f)) {
        _UpdateVolumeSettings(_curReso, fVoxelSize);
        _UpdateBlockSettings(_fuseBlockVoxelRatio,
            _renderBlockVoxelRatio);
        ResetAllResource();
    }
}

void
TSDFVolume::_CreateFuseBlockVolAndRelatedBuf(
    const uint3& reso, const uint ratio)
{
    uint32_t blockCount = (uint32_t)(reso.x * reso.y * reso.z / pow(ratio, 3));
    _fuseBlockVol.Destroy();
    _fuseBlockVol.Create(L"BlockVol", reso.x / ratio, reso.y / ratio,
        reso.z / ratio, 1, DXGI_FORMAT_R32_UINT);
    ComputeContext& cptCtx = ComputeContext::Begin(L"ResetBlockVol");
    cptCtx.SetRootSignature(_rootsig);
    _UpdatePerCallCB(cptCtx);
    cptCtx.SetConstantBuffer(1, _gpuCB.RootConstantBufferView());
    _CleanFuseBlockVol(cptCtx);
    _CleanRenderBlockVol(cptCtx);
    cptCtx.Finish(true);

    _updateBlocksBuf.Destroy();
    _updateQSize = blockCount;
    _updateBlocksBuf.Create(L"UpdateBlockQueue", _updateQSize, 4);

    _occupiedBlocksBuf.Destroy();
    _occupiedQSize = blockCount;
    _occupiedBlocksBuf.Create(L"OccupiedBlockQueue", _updateQSize, 4);

    _newFuseBlocksBuf.Destroy();
    _newQSize = (uint32_t)(_occupiedQSize * 0.3f);
    _newFuseBlocksBuf.Create(L"NewOccupiedBlockBuf",
        _newQSize, 4);

    _freedFuseBlocksBuf.Destroy();
    _freedQSize = (uint32_t)(_occupiedQSize * 0.3f);
    _freedFuseBlocksBuf.Create(L"FreedOccupiedBlockBuf",
        _freedQSize, 4);
}

void
TSDFVolume::_CreateRenderBlockVol(const uint3& reso, const uint ratio)
{
    _renderBlockVol.Destroy();
    _renderBlockVol.Create(L"RenderBlockVol", reso.x / ratio, reso.y / ratio,
        reso.z / ratio, 1, DXGI_FORMAT_R32_UINT);
}

void
TSDFVolume::_UpdateRenderCamData(const DirectX::XMMATRIX& mProj_T,
    const DirectX::XMMATRIX& mView_T)
{
    _cbPerFrame.mProjView = XMMatrixMultiply(mView_T, mProj_T);
    _cbPerFrame.mView = mView_T;
    _cbPerFrame.mViewInv = XMMatrixInverse(nullptr, mView_T);
}

void
TSDFVolume::_UpdateSensorData(const DirectX::XMMATRIX& mSensor_T)
{
    _cbPerFrame.mDepthViewInv = mSensor_T;
    _cbPerFrame.mDepthView = XMMatrixInverse(nullptr, mSensor_T);
}

void
TSDFVolume::_UpdateVolumeSettings(const uint3 reso, const float voxelSize)
{
    _cbStaled = true;
    uint3& xyz = _volParam->u3VoxelReso;
    if (xyz.x == reso.x && xyz.y == reso.y && xyz.z == reso.z &&
        voxelSize == _volParam->fVoxelSize) {
        return;
    }
    xyz = reso;
    _volParam->fVoxelSize = voxelSize;
    // TruncDist should be larger than (1 + 1/sqrt(2)) * voxelSize
    _volParam->fTruncDist = 1.7072f * voxelSize;
    _volParam->fInvVoxelSize = 1.f / voxelSize;
    _volParam->i3ResoVector = int3(1, reso.x, reso.x * reso.y);
    _volParam->f3HalfVoxelReso =
        float3(reso.x / 2.f, reso.y / 2.f, reso.z / 2.f);
    _volParam->f3HalfVolSize = float3(0.5f * reso.x * voxelSize,
        0.5f * reso.y * voxelSize, 0.5f * reso.z * voxelSize);
    uint3 u3BReso = uint3(reso.x - 2, reso.y - 2, reso.z - 2);
    _volParam->f3BoxMax = float3(0.5f * u3BReso.x * voxelSize,
        0.5f * u3BReso.y * voxelSize, 0.5f * u3BReso.z * voxelSize);
    _volParam->f3BoxMin = float3(-0.5f * u3BReso.x * voxelSize,
        -0.5f * u3BReso.y * voxelSize, -0.5f * u3BReso.z * voxelSize);
    // Update mXForm[0] for volume helper wireframe bounding box
    _cbPerCall.mXForm[0] = XMMatrixScaling(
        reso.x * voxelSize, reso.y * voxelSize, reso.z * voxelSize);
}

void
TSDFVolume::_UpdateBlockSettings(const uint fuseBlockVoxelRatio,
    const uint renderBlockVoxelRatio)
{
    _cbStaled = true;
    _volParam->uVoxelFuseBlockRatio = fuseBlockVoxelRatio;
    _volParam->uVoxelRenderBlockRatio = renderBlockVoxelRatio;
    _volParam->fFuseBlockSize = fuseBlockVoxelRatio * _volParam->fVoxelSize;
    _volParam->fRenderBlockSize = renderBlockVoxelRatio * _volParam->fVoxelSize;
}

void
TSDFVolume::_ClearBlockQueues(ComputeContext& cptCtx)
{
    static UINT ClearVal[4] = {};
    cptCtx.ClearUAV(_occupiedBlocksBuf, ClearVal);
    cptCtx.ResetCounter(_occupiedBlocksBuf);
    cptCtx.ClearUAV(_updateBlocksBuf, ClearVal);
    cptCtx.ResetCounter(_updateBlocksBuf);
    cptCtx.ClearUAV(_newFuseBlocksBuf, ClearVal);
    cptCtx.ResetCounter(_newFuseBlocksBuf);
    cptCtx.ClearUAV(_freedFuseBlocksBuf, ClearVal);
    cptCtx.ResetCounter(_freedFuseBlocksBuf);
}

void
TSDFVolume::_CleanTSDFVols(ComputeContext& cptCtx,
    const ManagedBuf::BufInterface& buf)
{
    cptCtx.SetPipelineState(_cptTSDFBufResetPSO[buf.type]);
    Bind(cptCtx, 2, 0, 2, buf.UAV);
    const uint3 xyz = _volParam->u3VoxelReso;
    cptCtx.Dispatch3D(xyz.x, xyz.y, xyz.z, THREAD_X, THREAD_Y, THREAD_Z);
}

void
TSDFVolume::_CleanFuseBlockVol(ComputeContext& cptCtx)
{
    cptCtx.SetPipelineState(_cptFuseBlockVolResetPSO);
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numSRVs> SRVs = _nullSRVs;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numUAVs> UAVs = _nullUAVs;
    UAVs[1] = _fuseBlockVol.GetUAV();
    Bind(cptCtx, 2, 0, numUAVs, UAVs.data());
    Bind(cptCtx, 3, 0, numSRVs, SRVs.data());
    const uint3 xyz = _volParam->u3VoxelReso;
    const uint ratio = _volParam->uVoxelFuseBlockRatio;
    cptCtx.Dispatch3D(xyz.x / ratio, xyz.y / ratio, xyz.z / ratio,
        THREAD_X, THREAD_Y, THREAD_Z);
}

void
TSDFVolume::_RefreshFuseBlockVol(ComputeContext& cptCtx)
{
    cptCtx.SetPipelineState(_cptFuseBlockVolRefreshPSO);
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numSRVs> SRVs = _nullSRVs;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numUAVs> UAVs = _nullUAVs;
    UAVs[1] = _fuseBlockVol.GetUAV();
    Bind(cptCtx, 2, 0, numUAVs, UAVs.data());
    Bind(cptCtx, 3, 0, numSRVs, SRVs.data());
    const uint3 xyz = _volParam->u3VoxelReso;
    const uint ratio = _volParam->uVoxelFuseBlockRatio;
    cptCtx.Dispatch3D(xyz.x / ratio, xyz.y / ratio, xyz.z / ratio,
        THREAD_X, THREAD_Y, THREAD_Z);
}

void
TSDFVolume::_CleanRenderBlockVol(ComputeContext& cptCtx)
{
    cptCtx.SetPipelineState(_cptRenderBlockVolResetPSO);
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numSRVs> SRVs = _nullSRVs;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numUAVs> UAVs = _nullUAVs;
    UAVs[0] = _renderBlockVol.GetUAV();
    Bind(cptCtx, 2, 0, numUAVs, UAVs.data());
    Bind(cptCtx, 3, 0, numSRVs, SRVs.data());
    const uint3 xyz = _volParam->u3VoxelReso;
    const uint ratio = _renderBlockVoxelRatio;
    cptCtx.Dispatch3D(xyz.x / ratio, xyz.y / ratio, xyz.z / ratio,
        THREAD_X, THREAD_Y, THREAD_Z);
}

void
TSDFVolume::_UpdateVolume(ComputeContext& cptCtx,
    const ManagedBuf::BufInterface& buf,
    ColorBuffer* pDepthTex, ColorBuffer* pWeightTex)
{
    VolumeStruct type = _useStepInfoTex ? kBlockVol : kVoxel;
    const uint3 xyz = _volParam->u3VoxelReso;
    if (_blockVolumeUpdate) {
        // Add blocks to UpdateBlockQueue from OccupiedBlockQueue
        Trans(cptCtx, _updateBlocksBuf, UAV);
        Trans(cptCtx, _fuseBlockVol, UAV);
        Trans(cptCtx, _occupiedBlocksBuf, csSRV);
        Trans(cptCtx, _indirectParams, IARG);
        {
            GPU_PROFILE(cptCtx, L"Occupied2UpdateQ");
            cptCtx.SetPipelineState(_cptBlockVolUpdate_Pass1);
            cptCtx.ResetCounter(_updateBlocksBuf);
            std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numUAVs> UAVs = _nullUAVs;
            UAVs[0] = _updateBlocksBuf.GetUAV();
            UAVs[1] = _fuseBlockVol.GetUAV();
            std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numSRVs> SRVs = _nullSRVs;
            SRVs[0] = _occupiedBlocksBuf.GetSRV();
            SRVs[1] = _occupiedBlocksBuf.GetCounterSRV(cptCtx);
            Bind(cptCtx, 2, 0, numUAVs, UAVs.data());
            Bind(cptCtx, 3, 0, numSRVs, SRVs.data());
            cptCtx.DispatchIndirect(_indirectParams, 0);
        }
        BeginTrans(cptCtx, _occupiedBlocksBuf, UAV);
        BeginTrans(cptCtx, _indirectParams, UAV);
        // Add blocks to UpdateBlockQueue from DepthMap
        Trans(cptCtx, _fuseBlockVol, UAV);
        Trans(cptCtx, *pDepthTex, psSRV | csSRV);
        Trans(cptCtx, *pWeightTex, csSRV);
        Trans(cptCtx, _updateBlocksBuf, UAV);
        {
            GPU_PROFILE(cptCtx, L"Depth2UpdateQ");
            cptCtx.SetPipelineState(_cptBlockVolUpdate_Pass2);
            // UAV barrier for _updateBlocksBuf is omit since the order of ops
            // on it does not matter before reading it during UpdateVoxel
            std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numUAVs> UAVs = _nullUAVs;
            UAVs[0] = _updateBlocksBuf.GetUAV();
            UAVs[1] = _fuseBlockVol.GetUAV();
            std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numSRVs> SRVs = _nullSRVs;
            SRVs[0] = pDepthTex->GetSRV();
            SRVs[1] = pWeightTex->GetSRV();
            Bind(cptCtx, 2, 0, numUAVs, UAVs.data());
            Bind(cptCtx, 3, 0, numSRVs, SRVs.data());
            cptCtx.Dispatch2D(
                _cbPerCall.i2DepthReso.x, _cbPerCall.i2DepthReso.y);
        }
        BeginTrans(cptCtx, _updateBlocksBuf, csSRV);
        BeginTrans(cptCtx, _fuseBlockVol, UAV);
        // Resolve UpdateBlockQueue for DispatchIndirect
        Trans(cptCtx, _indirectParams, UAV);
        Trans(cptCtx, _updateBlocksBuf, UAV);
        {
            GPU_PROFILE(cptCtx, L"UpdateQResolve");
            cptCtx.SetPipelineState(_cptBlockVolUpdate_Resolve);
            std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numUAVs> UAVs = _nullUAVs;
            UAVs[0] = _indirectParams.GetUAV();
            std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numSRVs> SRVs = _nullSRVs;
            SRVs[0] = _updateBlocksBuf.GetCounterSRV(cptCtx);
            Bind(cptCtx, 2, 0, numUAVs, UAVs.data());
            Bind(cptCtx, 3, 0, numSRVs, SRVs.data());
            cptCtx.Dispatch1D(1, WARP_SIZE);
        }
        //cptCtx.Flush();
        //cptCtx.SetRootSignature(_rootsig);
        //_UpdateAndBindConstantBuffer(cptCtx);
        // Update voxels in blocks from UpdateBlockQueue and create queues for
        // NewOccupiedBlocks and FreedOccupiedBlocks
        Trans(cptCtx, _occupiedBlocksBuf, UAV);
        Trans(cptCtx, _renderBlockVol, UAV);
        Trans(cptCtx, _fuseBlockVol, UAV);
        Trans(cptCtx, _newFuseBlocksBuf, UAV);
        Trans(cptCtx, _freedFuseBlocksBuf, UAV);
        Trans(cptCtx, _updateBlocksBuf, csSRV);
        Trans(cptCtx, _indirectParams, IARG);
        {
            GPU_PROFILE(cptCtx, L"UpdateVoxels");
            cptCtx.SetPipelineState(
                _cptBlockVolUpdate_Pass3[buf.type][_TGSize]);
            std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numUAVs> UAVs;
            UAVs[0] = buf.UAV[0];
            UAVs[1] = buf.UAV[1];
            UAVs[2] = _fuseBlockVol.GetUAV();
            UAVs[3] = _newFuseBlocksBuf.GetUAV();
            UAVs[4] = _freedFuseBlocksBuf.GetUAV();
            UAVs[5] = _occupiedBlocksBuf.GetUAV();
            UAVs[6] = _renderBlockVol.GetUAV();
            std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numSRVs> SRVs = _nullSRVs;
            SRVs[0] = pDepthTex->GetSRV();
            SRVs[1] = buf.SRV[0];
            SRVs[2] = buf.SRV[1];
            SRVs[3] = _updateBlocksBuf.GetSRV();
            Bind(cptCtx, 2, 0, numUAVs, UAVs.data());
            Bind(cptCtx, 3, 0, numSRVs, SRVs.data());
            cptCtx.DispatchIndirect(_indirectParams, 12);
        }
        BeginTrans(cptCtx, _fuseBlockVol, UAV);
        BeginTrans(cptCtx, _occupiedBlocksBuf, UAV);
        BeginTrans(cptCtx, _newFuseBlocksBuf, csSRV);
        BeginTrans(cptCtx, _freedFuseBlocksBuf, csSRV);
        // Read queue buffer counter back for debugging
        Trans(cptCtx, _indirectParams, UAV);
        if (_readBackGPUBufStatus) {
            Graphics::g_cmdListMngr.WaitForFence(_readBackFence);
            static D3D12_RANGE range = {0, 16};
            static D3D12_RANGE umapRange = {};
            _debugBuf.Map(&range, reinterpret_cast<void**>(&_readBackPtr));
            memcpy(_readBackData, _readBackPtr, 8 * sizeof(uint32_t));
            _debugBuf.Unmap(&umapRange);
            cptCtx.CopyBufferRegion(_debugBuf, 0,
                _occupiedBlocksBuf.GetCounterBuffer(), 0, 4);
            cptCtx.CopyBufferRegion(_debugBuf, 4,
                _updateBlocksBuf.GetCounterBuffer(), 0, 4);
            cptCtx.CopyBufferRegion(_debugBuf, 8,
                _newFuseBlocksBuf.GetCounterBuffer(), 0, 4);
            cptCtx.CopyBufferRegion(_debugBuf, 12,
                _freedFuseBlocksBuf.GetCounterBuffer(), 0, 4);



            cptCtx.CopyBufferRegion(_debugBuf, 16,
                _updateBlocksBuf.GetCounterBuffer(), 0, 4);
            cptCtx.CopyBufferRegion(_debugBuf, 20,
                _indirectParams, 12, 4);
            _readBackFence = cptCtx.Flush();
        }
        // Create indirect argument and params for Filling in
        // OccupiedBlockFreedSlot and AppendingNewOccupiedBlock
        Trans(cptCtx, _jobParamBuf, UAV);
        {
            GPU_PROFILE(cptCtx, L"OccupiedQPrepare");
            cptCtx.SetPipelineState(_cptBlockQUpdate_Prepare);
            std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numUAVs> UAVs = _nullUAVs;
            UAVs[0] = _newFuseBlocksBuf.GetCounterUAV(cptCtx);
            UAVs[1] = _freedFuseBlocksBuf.GetCounterUAV(cptCtx);
            UAVs[2] = _indirectParams.GetUAV();
            UAVs[3] = _jobParamBuf.GetUAV();
            std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numSRVs> SRVs = _nullSRVs;
            SRVs[0] = _occupiedBlocksBuf.GetCounterSRV(cptCtx);
            Bind(cptCtx, 2, 0, numUAVs, UAVs.data());
            Bind(cptCtx, 3, 0, numSRVs, SRVs.data());
            cptCtx.SetConstantBuffer(1, _gpuCB.RootConstantBufferView());
            cptCtx.Dispatch1D(1, WARP_SIZE);
        }
        // Filling in NewOccupiedBlocks into FreeSlots in OccupiedBlockQueue
        Trans(cptCtx, _occupiedBlocksBuf, UAV);
        Trans(cptCtx, _newFuseBlocksBuf, csSRV);
        Trans(cptCtx, _freedFuseBlocksBuf, csSRV);
        Trans(cptCtx, _jobParamBuf, csSRV);
        Trans(cptCtx, _indirectParams, IARG);
        {
            GPU_PROFILE(cptCtx, L"FreedQFillIn");
            cptCtx.SetPipelineState(_cptBlockQUpdate_Pass1);
            std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numUAVs> UAVs = _nullUAVs;
            UAVs[0] = _occupiedBlocksBuf.GetUAV();
            std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numSRVs> SRVs = _nullSRVs;
            SRVs[0] = _newFuseBlocksBuf.GetSRV();
            SRVs[1] = _freedFuseBlocksBuf.GetSRV();
            SRVs[2] = _jobParamBuf.GetSRV();
            Bind(cptCtx, 2, 0, numUAVs, UAVs.data());
            Bind(cptCtx, 3, 0, numSRVs, SRVs.data());
            cptCtx.DispatchIndirect(_indirectParams, 24);
        }
        // Appending new occupied blocks to OccupiedBlockQueue
        {
            GPU_PROFILE(cptCtx, L"OccupiedQAppend");
            cptCtx.SetPipelineState(_cptBlockQUpdate_Pass2);
            std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numUAVs> UAVs = _nullUAVs;
            UAVs[0] = _occupiedBlocksBuf.GetUAV();
            std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numSRVs> SRVs = _nullSRVs;
            SRVs[0] = _newFuseBlocksBuf.GetSRV();
            SRVs[1] = _jobParamBuf.GetSRV();
            Bind(cptCtx, 2, 0, numUAVs, UAVs.data());
            Bind(cptCtx, 3, 0, numSRVs, SRVs.data());
            cptCtx.DispatchIndirect(_indirectParams, 36);
        }
        BeginTrans(cptCtx, _occupiedBlocksBuf, UAV);
        BeginTrans(cptCtx, _jobParamBuf, csSRV);
        BeginTrans(cptCtx, _newFuseBlocksBuf, UAV);
        BeginTrans(cptCtx, _freedFuseBlocksBuf, csSRV);
        // Resolve OccupiedBlockQueue for next VoxelUpdate DispatchIndirect
        Trans(cptCtx, _indirectParams, UAV);
        {
            GPU_PROFILE(cptCtx, L"OccupiedQResolve");
            cptCtx.SetPipelineState(_cptBlockQUpdate_Resolve);
            std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numUAVs> UAVs = _nullUAVs;
            UAVs[0] = _indirectParams.GetUAV();
            std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numSRVs> SRVs = _nullSRVs;
            SRVs[0] = _occupiedBlocksBuf.GetCounterSRV(cptCtx);
            Bind(cptCtx, 2, 0, numUAVs, UAVs.data());
            Bind(cptCtx, 3, 0, numSRVs, SRVs.data());
            cptCtx.Dispatch1D(1, WARP_SIZE);
        }
    } else {
        GPU_PROFILE(cptCtx, L"Volume Updating");
        _CleanRenderBlockVol(cptCtx);
        cptCtx.SetPipelineState(_cptUpdatePSO[buf.type][type]);

        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numUAVs> UAVs = _nullUAVs;
        UAVs[0] = buf.UAV[0];
        UAVs[1] = buf.UAV[1];
        UAVs[2] = _renderBlockVol.GetUAV();
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numSRVs> SRVs = _nullSRVs;
        SRVs[0] = pDepthTex->GetSRV();
        SRVs[1] = buf.SRV[0];
        SRVs[2] = buf.SRV[1];
        Bind(cptCtx, 2, 0, numUAVs, UAVs.data());
        Bind(cptCtx, 3, 0, numSRVs, SRVs.data());
        cptCtx.Dispatch3D(xyz.x, xyz.y, xyz.z, THREAD_X, THREAD_Y, THREAD_Z);
    }
}

void
TSDFVolume::_RenderNearFar(GraphicsContext& gfxCtx, bool toSurface)
{
    gfxCtx.SetPipelineState(toSurface
        ? _gfxStepInfoSensorPSO[_noInstance]
        : _gfxStepInfoVCamPSO[_noInstance]);
    gfxCtx.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numUAVs> UAVs = _nullUAVs;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numSRVs> SRVs = _nullSRVs;
    SRVs[1] = _renderBlockVol.GetSRV();
    Bind(gfxCtx, 2, 0, numUAVs, UAVs.data());
    Bind(gfxCtx, 3, 0, numSRVs, SRVs.data());

    const uint3 xyz = _volParam->u3VoxelReso;
    const uint ratio = _volParam->uVoxelRenderBlockRatio;
    uint BrickCount = xyz.x * xyz.y * xyz.z / ratio / ratio / ratio;
    if (_noInstance) {
        gfxCtx.Draw(BrickCount * 16);
    } else {
        gfxCtx.SetIndexBuffer(_cubeTriangleStripIB.IndexBufferView());
        gfxCtx.DrawIndexedInstanced(
            CUBE_TRIANGLESTRIP_LENGTH, BrickCount, 0, 0, 0);
    }
}

void
TSDFVolume::_RenderVolume(GraphicsContext& gfxCtx,
    const ManagedBuf::BufInterface& buf, bool toOutTex)
{
    VolumeStruct type = _useStepInfoTex ? kBlockVol : kVoxel;
    gfxCtx.SetPipelineState(toOutTex
        ? _gfxSensorRenderPSO[buf.type][type][_filterType]
        : _gfxVCamRenderPSO[buf.type][type][_filterType]);
    gfxCtx.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numUAVs> UAVs = _nullUAVs;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numSRVs> SRVs = _nullSRVs;
    SRVs[0] = buf.SRV[0];
    if (_useStepInfoTex) {
        SRVs[1] = toOutTex ?
            _nearFarForProcess.GetSRV() : _nearFarForVisual.GetSRV();
        SRVs[2] = _renderBlockVol.GetSRV();
    }
    Bind(gfxCtx, 2, 0, numUAVs, UAVs.data());
    Bind(gfxCtx, 3, 0, numSRVs, SRVs.data());

    gfxCtx.Draw(3);
}

void
TSDFVolume::_RenderHelperWireframe(GraphicsContext& gfxCtx)
{
    gfxCtx.SetPipelineState(_gfxHelperWireframePSO);
    gfxCtx.DrawIndexedInstanced(CUBE_LINESTRIP_LENGTH, 3, 0, 0, 0);
}

void
TSDFVolume::_RenderBrickGrid(GraphicsContext& gfxCtx)
{
    gfxCtx.SetPipelineState(_gfxDebugFuseBlockPSO);
    _UpdateAndBindConstantBuffer(gfxCtx);
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numUAVs> UAVs = _nullUAVs;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numSRVs> SRVs = _nullSRVs;
    SRVs[1] = _fuseBlockVol.GetSRV();
    Bind(gfxCtx, 2, 0, numUAVs, UAVs.data());
    Bind(gfxCtx, 3, 0, numSRVs, SRVs.data());
    const uint3 xyz = _volParam->u3VoxelReso;
    const uint ratio = _volParam->uVoxelFuseBlockRatio;
    uint BrickCount = xyz.x * xyz.y * xyz.z / ratio / ratio / ratio;
    gfxCtx.DrawIndexedInstanced(CUBE_LINESTRIP_LENGTH, BrickCount, 0, 0, 0);
}

void
TSDFVolume::_UpdatePerCallCB(CommandContext& cmdCtx)
{
    if (_cbStaled) {
        memcpy(_pUploadCB->DataPtr, &_cbPerCall, sizeof(PerCallDataCB));
        cmdCtx.CopyBufferRegion(_gpuCB, 0, _pUploadCB->Buffer,
            _pUploadCB->Offset, sizeof(PerCallDataCB));
        _cbStaled = false;
    }
}

template<class T>
void
TSDFVolume::_UpdateAndBindConstantBuffer(T& ctx)
{
    _UpdatePerCallCB(ctx);
    ctx.SetConstantBuffer(1, _gpuCB.RootConstantBufferView());
    BindCB(ctx, 0, sizeof(_cbPerFrame), (void*)&_cbPerFrame);
}