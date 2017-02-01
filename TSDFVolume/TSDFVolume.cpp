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
using State = D3D12_RESOURCE_STATES;
enum VSMode {
    kInstance = 0,
    kIndexed,
    kNumVSMode,
};

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
const UINT kCam    = TSDFVolume::kNumCam;
const UINT kTG     = TSDFVolume::kTG;

DXGI_FORMAT ColorBufFormat;
DXGI_FORMAT DepthBufFormat;
DXGI_FORMAT DepthMapFormat;

const UINT numSRVs = 6;
const UINT numUAVs = 7;

const DXGI_FORMAT _stepInfoTexFormat = DXGI_FORMAT_R16G16_FLOAT;

VSMode _vsMode = kInstance;
bool _typedLoadSupported = false;

bool _useStepInfoTex = true;
bool _stepInfoDebug = false;
bool _blockVolumeUpdate = true;
bool _readBackGPUBufStatus = true;

bool _cbStaled = true;

// Define the vertex input layout.
D3D12_INPUT_ELEMENT_DESC inputElementDescs[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,
    D3D12_APPEND_ALIGNED_ELEMENT,
    D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}
};

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

// PSO for prepare GPU matrix buffer
ComputePSO _cptPSOprepareMatrix;
// PSO for naive updating voxels
ComputePSO _cptPSOvoxelUpdate[kBuf][kStruct];
// PSOs for block voxel updating
ComputePSO _cptPSOupdateQFromOccupiedQ;
ComputePSO _cptPSOupdateQFromDepthMap;
ComputePSO _cptPSOupdateQResolve;
ComputePSO _cptPSOblockUpdate[kBuf][kTG];
ComputePSO _cptPSOprepareNewFreeQ;
ComputePSO _cptPSOfillFreeQ;
ComputePSO _cptPSOappendOccupiedQ;
ComputePSO _cptPSOoccupiedQResolve;
// PSO for occupied queue defragmentation
ComputePSO _cptPSOoccupiedQDefragment;
// PSOs for rendering
GraphicsPSO _gfxPSORaycast[kBuf][kStruct][kFilter][kCam];
GraphicsPSO _gfxPSORayNearFar[kNumVSMode][kCam];
GraphicsPSO _gfxPSODebugRayNearFar;
GraphicsPSO _gfxPSOHelperWireframe;
// PSOs for reseting
ComputePSO _cptPSOresetFuseBlockVol;
ComputePSO _cptPSOresetRenderBlockVol;
ComputePSO _cptPSOresetTSDFBuf[kBuf];

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

#define CreatePSO(ObjName, Shader)\
ObjName.SetRootSignature(_rootsig);\
ObjName.SetComputeShader(Shader->GetBufferPointer(), Shader->GetBufferSize());\
ObjName.Finalize();

#define InitializePSO(ObjName)\
ObjName.SetRootSignature(_rootsig);\
ObjName.SetInputLayout(0, nullptr);\
ObjName.SetRasterizerState(Graphics::g_RasterizerDefault);\
ObjName.SetBlendState(Graphics::g_BlendDisable);\
ObjName.SetDepthStencilState(Graphics::g_DepthStateDisabled);\
ObjName.SetSampleMask(UINT_MAX);\
ObjName.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);

void _CreateCptPSO_PrepareMatrix() {
    HRESULT hr; ComPtr<ID3DBlob> cs;
    D3D_SHADER_MACRO macro[] = {{"__hlsl", "1"}, {nullptr, nullptr}};
    V(_Compile(L"PrepareMatrixBuf_cs", macro, &cs));
    CreatePSO(_cptPSOprepareMatrix, cs);
}

void _CreateCptPSO_VoxelUpdate(
    ManagedBuf::Type bufType, TSDFVolume::VolumeStruct structType) {
    HRESULT hr; ComPtr<ID3DBlob> cs;
    D3D_SHADER_MACRO macro[] = {      /*0*/ {"__hlsl",    "1"},
        /*1*/ {"ENABLE_BRICKS", "0"}, /*2*/ {"TYPED_UAV", "0"},
        /*3*/ {"NO_TYPED_LOAD", "0"}, {nullptr, nullptr}};
    if (!_typedLoadSupported) macro[3].Definition = "1";
    if (structType == TSDFVolume::kBlockVol) macro[1].Definition = "1";
    if (bufType == ManagedBuf::kTypedBuffer) macro[2].Definition = "1";
    V(_Compile(L"VolumeUpdate_cs", macro, &cs));
    CreatePSO(_cptPSOvoxelUpdate[bufType][structType], cs);
}

void _CreateCptPSO_UpdateQFromOccupiedQ() {
    HRESULT hr; ComPtr<ID3DBlob> cs;
    D3D_SHADER_MACRO macro[] = {{"__hlsl", "1"},
        {"UPDATE_FROM_OCCUPIEDQ", "1"}, {nullptr, nullptr}};
    V(_Compile(L"BlockQueueCreate_cs", macro, &cs));
    CreatePSO(_cptPSOupdateQFromOccupiedQ, cs);
}

void _CreateCptPSO_UpdateQFromDepthMap() {
    HRESULT hr; ComPtr<ID3DBlob> cs;
    D3D_SHADER_MACRO macro[] = {{"__hlsl", "1"},
       {"UPDATE_FROM_DEPTHMAP", "1"}, {nullptr, nullptr}};
    V(_Compile(L"BlockQueueCreate_cs", macro, &cs));
    CreatePSO(_cptPSOupdateQFromDepthMap, cs);
}

void _CreateCptPSO_UpdateQResolve() {
    HRESULT hr; ComPtr<ID3DBlob> cs;
    D3D_SHADER_MACRO macro[] = {{"__hlsl", "1"}, {nullptr, nullptr}};
    V(_Compile(L"BlockQueueResolve_cs", macro, &cs));
    CreatePSO(_cptPSOupdateQResolve, cs);
}

void _CreateCptPSO_BlockUpdate(
    ManagedBuf::Type bufType, TSDFVolume::ThreadGroup tg) {
    HRESULT hr; ComPtr<ID3DBlob> cs;
    D3D_SHADER_MACRO macro[] = {      /*0*/ {"__hlsl",        "1"},
        /*1*/ {"TYPED_UAV",     "0"}, /*2*/ {"NO_TYPED_LOAD", "0"},
        /*3*/ {"THREAD_DIM",    "4"}, {nullptr, nullptr}};
    if (bufType == ManagedBuf::kTypedBuffer) macro[1].Definition = "1";
    if (!_typedLoadSupported)                macro[2].Definition = "1";
    if (tg == TSDFVolume::k512)              macro[3].Definition = "8";
    V(_Compile(L"BlocksUpdate_cs", macro, &cs));
    CreatePSO(_cptPSOblockUpdate[bufType][tg], cs);
}

void _CreateCptPSO_PrepareNewFreeQ() {
    HRESULT hr; ComPtr<ID3DBlob> cs;
    D3D_SHADER_MACRO macro[] = 
        {{"__hlsl", "1"}, {"PREPARE", "1"}, {nullptr, nullptr}};
    V(_Compile(L"OccupiedQueueUpdate_cs", macro, &cs));
    CreatePSO(_cptPSOprepareNewFreeQ, cs);
}

void _CreateCptPSO_FillFreeQ() {
    HRESULT hr; ComPtr<ID3DBlob> cs;
    D3D_SHADER_MACRO macro[] =
        {{"__hlsl", "1"}, {"FILL_FREEQ", "1"}, {nullptr, nullptr}};
    V(_Compile(L"OccupiedQueueUpdate_cs", macro, &cs));
    CreatePSO(_cptPSOfillFreeQ, cs);
}

void _CreateCptPSO_AppendOccupiedQ() {
    HRESULT hr; ComPtr<ID3DBlob> cs;
    D3D_SHADER_MACRO macro[] =
        {{"__hlsl", "1"}, {"APPEND_OCCUPIEDQ", "1"}, {nullptr, nullptr}};
    V(_Compile(L"OccupiedQueueUpdate_cs", macro, &cs));
    CreatePSO(_cptPSOappendOccupiedQ, cs);
}

void _CreateCptPSO_OccupiedQResolve() {
    HRESULT hr; ComPtr<ID3DBlob> cs;
    D3D_SHADER_MACRO macro[] =
        {{"__hlsl", "1"}, {"RESOLVE", "1"}, {nullptr, nullptr}};
    V(_Compile(L"OccupiedQueueUpdate_cs", macro, &cs));
    CreatePSO(_cptPSOoccupiedQResolve, cs);
}

void _CreateCptPSO_OccupiedQDefragment() {
    HRESULT hr; ComPtr<ID3DBlob> cs;
    D3D_SHADER_MACRO macro[] = {{"__hlsl", "1"}, {nullptr, nullptr}};
    V(_Compile(L"QueueDefragment_cs", macro, &cs));
    CreatePSO(_cptPSOoccupiedQDefragment, cs);
}

void _CreateCptPSO_ResetFuseBlockVol() {
    HRESULT hr; ComPtr<ID3DBlob> cs;
    D3D_SHADER_MACRO macro[] = {{"__hlsl", "1"},
        {"FUSEBLOCKVOL_RESET", "1"}, {nullptr, nullptr}};
    V(_Compile(L"VolumeReset_cs", macro, &cs));
    CreatePSO(_cptPSOresetFuseBlockVol, cs);
}

void _CreateCptPSO_ResetRenderBlockVol() {
    HRESULT hr; ComPtr<ID3DBlob> cs;
    D3D_SHADER_MACRO macro[] = {{"__hlsl", "1"},
    {"RENDERBLOCKVOL_RESET", "1"}, {nullptr, nullptr}};
    V(_Compile(L"VolumeReset_cs", macro, &cs));
    CreatePSO(_cptPSOresetRenderBlockVol, cs);
}

void _CreateCptPSO_ResetTSDFBuf(ManagedBuf::Type buf) {
    HRESULT hr; ComPtr<ID3DBlob> cs;
    D3D_SHADER_MACRO macro[] = {{"__hlsl", "1"}, {"TSDFVOL_RESET", "1"},
        {"TYPED_UAV", "0"}, {nullptr, nullptr}};
    if (buf == ManagedBuf::kTypedBuffer) macro[2].Definition = "1";
    V(_Compile(L"VolumeReset_cs", macro, &cs));
    CreatePSO(_cptPSOresetTSDFBuf[buf], cs);
}

void _CreateGfxPSO_RayCast(TSDFVolume::CamType cam, ManagedBuf::Type buf,
    TSDFVolume::VolumeStruct stru, TSDFVolume::FilterType filter) {
    HRESULT hr; ComPtr<ID3DBlob> vs, ps;
    D3D_SHADER_MACRO macro[] = {      /*0*/ {"__hlsl",     "1"},
        /*1*/ {"ENABLE_BRICKS", "0"}, /*2*/ {"TYPED_UAV",  "0"},
        /*3*/ {"FILTER_READ",   "0"}, /*4*/ {"FOR_SENSOR", "0"},
        /*5*/ {"FOR_VCAMERA",   "0"}, {nullptr, nullptr}};
    if (stru == TSDFVolume::kBlockVol)   macro[1].Definition = "1";
    if (buf == ManagedBuf::kTypedBuffer) macro[2].Definition = "1";
    switch (filter) {
    case TSDFVolume::kLinearFilter:      macro[3].Definition = "1"; break;
    case TSDFVolume::kSamplerLinear:     macro[3].Definition = "2"; break;
    case TSDFVolume::kSamplerAniso:      macro[3].Definition = "3"; break;
    }
    switch (cam) {
    case TSDFVolume::kVirtual:           macro[5].Definition = "1"; break;
    case TSDFVolume::kSensor:            macro[4].Definition = "1"; break;
    }
    V(_Compile(L"RayCast_vs", macro, &vs));
    V(_Compile(L"RayCast_ps", macro, &ps));
    GraphicsPSO& PSO = _gfxPSORaycast[buf][stru][filter][cam];
    InitializePSO(PSO);
    PSO.SetDepthStencilState(cam == TSDFVolume::kSensor ?
        Graphics::g_DepthStateDisabled : Graphics::g_DepthStateReadWrite);
    PSO.SetRenderTargetFormats(1, &DepthMapFormat,
        cam == TSDFVolume::kSensor ? DXGI_FORMAT_UNKNOWN : DepthBufFormat);
    PSO.SetVertexShader(vs->GetBufferPointer(), vs->GetBufferSize());
    PSO.SetPixelShader(ps->GetBufferPointer(), ps->GetBufferSize());
    PSO.Finalize();
}

void _CreateGfxPSO_RayNearFar(VSMode vsMode, TSDFVolume::CamType cam) {
    HRESULT hr; ComPtr<ID3DBlob> vs, ps;
    D3D_SHADER_MACRO macro[] = {{"__hlsl", "1"},
        {"FOR_VCAMERA", "0"}, {"FOR_SENSOR", "0"}, {nullptr, nullptr}};
    std::wstring vsName;
    vsName = vsMode == kInstance ? L"StepInfo_vs" : L"StepInfoNoInstance_vs";
    if (cam == TSDFVolume::kVirtual)    macro[1].Definition = "1";
    if (cam == TSDFVolume::kSensor)     macro[2].Definition = "1";
    V(_Compile(vsName.c_str(), macro, &vs));
    V(_Compile(L"StepInfo_ps", macro, &ps));
    GraphicsPSO& PSO = _gfxPSORayNearFar[vsMode][cam];
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
    InitializePSO(PSO);
    PSO.SetRasterizerState(Graphics::g_RasterizerTwoSided);
    PSO.SetBlendState(blendDesc);
    PSO.SetPrimitiveRestart(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF);
    if (vsMode == kInstance) {
        PSO.SetInputLayout(_countof(inputElementDescs), inputElementDescs);
    }
    PSO.SetRenderTargetFormats(1, &_stepInfoTexFormat, DXGI_FORMAT_UNKNOWN);
    PSO.SetVertexShader(vs->GetBufferPointer(), vs->GetBufferSize());
    PSO.SetPixelShader(ps->GetBufferPointer(), ps->GetBufferSize());
    PSO.Finalize();
}

void _CreateGfxPSO_DebugRayNearFar() {
    HRESULT hr; ComPtr<ID3DBlob> vs, ps;
    D3D_SHADER_MACRO macro[] = {{"__hlsl", "1"}, {"FOR_VCAMERA", "1"},
        {"DEBUG_VIEW", "1"}, {nullptr, nullptr}};
    V(_Compile(L"StepInfo_vs", macro, &vs));
    V(_Compile(L"StepInfo_ps", macro, &ps));
    GraphicsPSO& PSO = _gfxPSODebugRayNearFar;
    D3D12_RASTERIZER_DESC rastDesc = Graphics::g_RasterizerTwoSided;
    rastDesc.FillMode = D3D12_FILL_MODE_WIREFRAME;
    InitializePSO(PSO);
    PSO.SetRasterizerState(rastDesc);
    PSO.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE);
    PSO.SetPrimitiveRestart(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF);
    PSO.SetInputLayout(_countof(inputElementDescs), inputElementDescs);
    PSO.SetDepthStencilState(Graphics::g_DepthStateReadOnly);
    PSO.SetRenderTargetFormats(1, &ColorBufFormat, DepthBufFormat);
    PSO.SetVertexShader(vs->GetBufferPointer(), vs->GetBufferSize());
    PSO.SetPixelShader(ps->GetBufferPointer(), ps->GetBufferSize());
    PSO.Finalize();
}

void _CreateGfxPSO_HelperWireframe() {
    HRESULT hr; ComPtr<ID3DBlob> vs, ps;
    D3D_SHADER_MACRO macro[] = {{"__hlsl", "1"}, {nullptr, nullptr}};
    V(_Compile(L"HelperWireframe_vs", macro, &vs));
    V(_Compile(L"HelperWireframe_ps", macro, &ps));
    GraphicsPSO& PSO = _gfxPSOHelperWireframe;
    D3D12_RASTERIZER_DESC rastDesc = Graphics::g_RasterizerTwoSided;
    rastDesc.FillMode = D3D12_FILL_MODE_WIREFRAME;
    InitializePSO(PSO);
    PSO.SetRasterizerState(rastDesc);
    PSO.SetPrimitiveRestart(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF);
    PSO.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE);
    PSO.SetDepthStencilState(Graphics::g_DepthStateReadOnly);
    PSO.SetInputLayout(_countof(inputElementDescs), inputElementDescs);
    PSO.SetRenderTargetFormats(1, &ColorBufFormat, DepthBufFormat);
    PSO.SetVertexShader(vs->GetBufferPointer(), vs->GetBufferSize());
    PSO.SetPixelShader(ps->GetBufferPointer(), ps->GetBufferSize());
    PSO.Finalize();
}
#undef InitializePSO
#undef CreatePSO

void _CreateResource()
{
    HRESULT hr;

    // Create PSO for volume update and volume render
    ColorBufFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
    DepthBufFormat = Graphics::g_SceneDepthBuffer.GetFormat();
    DepthMapFormat = DXGI_FORMAT_R16_UNORM;

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
    : _volBuf(XMUINT3(512, 512, 512)),
    _nearFarTex{XMVectorSet(MAX_DEPTH, 0, 0, 0),XMVectorSet(MAX_DEPTH, 0, 0, 0)}
{
    _volParam = &_cbPerCall.vParam;
    _volParam->fMaxWeight = 20.f;
    _volParam->fVoxelSize = 1.f / 100.f;
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
    _debugBuf.Create(L"DebugBuf", 4, 4);

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

    _nearFarTex[kSensor].Create(L"NearFarTex_Sensor",
        depthReso.x, depthReso.y, 0, _stepInfoTexFormat);

    // Create matrix buffer
    matrix m[] = {XMMatrixIdentity(), XMMatrixIdentity()};
    _sensorMatrixBuf.Create(L"MatrixBuffer", 2, sizeof(matrix), (void*)m);

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
    _nearFarTex[kSensor].Destroy();
    _nearFarTex[kVirtual].Destroy();
    _cubeVB.Destroy();
    _cubeTriangleStripIB.Destroy();
    _cubeLineStripIB.Destroy();
    _sensorMatrixBuf.Destroy();
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

    _nearFarTex[kVirtual].Destroy();
    _nearFarTex[kVirtual].Create(L"NearFarTex_VCam", width, height, 1,
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
    _CleanTSDFVols(cptCtx, _curBuf);
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
    static float fAnimTime = 0.f;
    fAnimTime += (float)Core::g_deltaTime;
    _cbPerFrame.fTime = fAnimTime;
    _UpdateRenderCamData(mVCamProj_T, mVCamView_T);
    ManagedBuf::BufInterface newBufInterface = _volBuf.GetResource();
    _curBuf = newBufInterface;

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
TSDFVolume::UpdateGPUMatrixBuf(ComputeContext& cptCtx, StructuredBuffer* buf)
{
    GPU_PROFILE(cptCtx, L"UpdateGPUMatrix");
    Trans(cptCtx, _sensorMatrixBuf, UAV);
    Trans(cptCtx, *buf, csSRV);
    if (!_cptPSOprepareMatrix.GetPipelineStateObject())
        _CreateCptPSO_PrepareMatrix();
    cptCtx.SetPipelineState(_cptPSOprepareMatrix);
    cptCtx.SetRootSignature(_rootsig);
    _UpdateAndBindConstantBuffer(cptCtx);
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numUAVs> UAVs = _nullUAVs;
    UAVs[0] = _sensorMatrixBuf.GetUAV();
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numSRVs> SRVs = _nullSRVs;
    SRVs[0] = buf->GetSRV();
    Bind(cptCtx, 2, 0, numUAVs, UAVs.data());
    Bind(cptCtx, 3, 0, numSRVs, SRVs.data());
    cptCtx.Dispatch1D(1);
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
        if (!_cptPSOoccupiedQDefragment.GetPipelineStateObject())
            _CreateCptPSO_OccupiedQDefragment();
        cptCtx.SetPipelineState(_cptPSOoccupiedQDefragment);
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
    Trans(cptCtx, *_curBuf.resource[0], UAV);
    Trans(cptCtx, *_curBuf.resource[1], UAV);
    _UpdateVolume(cptCtx, _curBuf, pDepthTex, pWeightTex);
    BeginTrans(cptCtx, *_curBuf.resource[0], psSRV);
    BeginTrans(cptCtx, *_curBuf.resource[1], UAV);
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
        if (pDepthOut) Trans(gfxCtx, _nearFarTex[kSensor], RTV);
        if (pVisDepthOut) Trans(gfxCtx, _nearFarTex[kVirtual], RTV);
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
        gfxCtx.ClearColor(_nearFarTex[kSensor]);
        gfxCtx.SetRenderTarget(_nearFarTex[kSensor].GetRTV());
        _RenderNearFar(gfxCtx, kSensor);
        // Early submit to keep GPU busy
        gfxCtx.Flush();
        gfxCtx.SetRootSignature(_rootsig);
        _UpdateAndBindConstantBuffer(gfxCtx);
        gfxCtx.SetVertexBuffer(0, _cubeVB.VertexBufferView());
    }
    if (pVisDepthOut && _useStepInfoTex) {
        gfxCtx.SetViewport(_depthVisViewPort);
        gfxCtx.SetScisor(_depthVisSissorRect);
        BeginTrans(gfxCtx, _nearFarTex[kSensor], psSRV);
        GPU_PROFILE(gfxCtx, L"NearFar_Visu");
        gfxCtx.ClearColor(_nearFarTex[kVirtual]);
        gfxCtx.SetRenderTarget(_nearFarTex[kVirtual].GetRTV());
        _RenderNearFar(gfxCtx, kVirtual);
    }
    if (pDepthOut) {
        if (_useStepInfoTex) {
            BeginTrans(gfxCtx, _nearFarTex[kVirtual], psSRV);
            Trans(gfxCtx, _nearFarTex[kSensor], psSRV);
        }
        Trans(gfxCtx, *_curBuf.resource[0], psSRV);
        {
            GPU_PROFILE(gfxCtx, L"Volume_Proc");
            gfxCtx.ClearColor(*pDepthOut);
            gfxCtx.SetViewport(_depthViewPort);
            gfxCtx.SetScisor(_depthSissorRect);
            gfxCtx.SetRenderTarget(pDepthOut->GetRTV());
            _RenderVolume(gfxCtx, _curBuf, kSensor);
        }
        if (_useStepInfoTex) {
            BeginTrans(gfxCtx, _nearFarTex[kSensor], RTV);
        }
    }
    if (pVisDepthOut) {
        if (_useStepInfoTex) {
            Trans(gfxCtx, _nearFarTex[kVirtual], psSRV);
        }
        Trans(gfxCtx, *_curBuf.resource[0], psSRV);
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
            _RenderVolume(gfxCtx, _curBuf, kVirtual);
        }
        if (_useStepInfoTex) {
            BeginTrans(gfxCtx, _nearFarTex[kVirtual], RTV);
        }
    }
    BeginTrans(gfxCtx, *_curBuf.resource[0], UAV);
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
    if (_useStepInfoTex && _stepInfoDebug) {
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
        VolumeStruct stru = _useStepInfoTex ? kBlockVol : kVoxel;
        _CreateCptPSO_VoxelUpdate(_curBuf.type, stru);
        _CreateCptPSO_UpdateQFromOccupiedQ();
        _CreateCptPSO_UpdateQFromDepthMap();
        _CreateCptPSO_UpdateQResolve();
        _CreateCptPSO_BlockUpdate(_curBuf.type, _TGSize);
        _CreateCptPSO_PrepareNewFreeQ();
        _CreateCptPSO_FillFreeQ();
        _CreateCptPSO_AppendOccupiedQ();
        _CreateCptPSO_OccupiedQResolve();
        _CreateCptPSO_OccupiedQDefragment();
        _CreateCptPSO_ResetFuseBlockVol();
        _CreateCptPSO_ResetRenderBlockVol();
        _CreateCptPSO_ResetTSDFBuf(_curBuf.type);
        _CreateGfxPSO_RayCast(kSensor, _curBuf.type, stru, _filterType);
        _CreateGfxPSO_RayCast(kVirtual, _curBuf.type, stru, _filterType);
        _CreateGfxPSO_RayNearFar(_vsMode, kSensor);
        _CreateGfxPSO_RayNearFar(_vsMode, kVirtual);
        _CreateGfxPSO_DebugRayNearFar();
        _CreateGfxPSO_HelperWireframe();
        _CreateCptPSO_PrepareMatrix();
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
    }
    Separator();
    RadioButton("Instance Cube", (int*)&_vsMode, kInstance); SameLine();
    RadioButton("Vertex Cube", (int*)&_vsMode, kIndexed);
    Separator();
    Checkbox("StepInfoTex", &_useStepInfoTex); SameLine();
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
    RadioButton("128##X", (int*)&uiReso.x, 128); SameLine();
    RadioButton("256##X", (int*)&uiReso.x, 256); SameLine();
    RadioButton("384##X", (int*)&uiReso.x, 384); SameLine();
    RadioButton("512##X", (int*)&uiReso.x, 512);

    AlignFirstTextHeightToWidgets();
    Text("Y:"); SameLine();
    RadioButton("128##Y", (int*)&uiReso.y, 128); SameLine();
    RadioButton("256##Y", (int*)&uiReso.y, 256); SameLine();
    RadioButton("384##Y", (int*)&uiReso.y, 384); SameLine();
    RadioButton("512##Y", (int*)&uiReso.y, 512);

    AlignFirstTextHeightToWidgets();
    Text("Z:"); SameLine();
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
        &fVoxelSize, 1.f / 256.f, 10.f / 256.f)) {
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
    // TruncDist should be larger than (1 + sqrt(3)/2) * voxelSize
    _volParam->fTruncDist = 1.875f * voxelSize;
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
    if (!_cptPSOresetTSDFBuf[buf.type].GetPipelineStateObject())
        _CreateCptPSO_ResetTSDFBuf(buf.type);
    cptCtx.SetPipelineState(_cptPSOresetTSDFBuf[buf.type]);
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numSRVs> SRVs = _nullSRVs;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numUAVs> UAVs = _nullUAVs;
    UAVs[0] = buf.UAV[0];
    UAVs[1] = buf.UAV[1];
    Bind(cptCtx, 2, 0, numUAVs, UAVs.data());
    Bind(cptCtx, 3, 0, numSRVs, SRVs.data());
    const uint3 xyz = _volParam->u3VoxelReso;
    cptCtx.Dispatch3D(xyz.x, xyz.y, xyz.z, THREAD_X, THREAD_Y, THREAD_Z);
}

void
TSDFVolume::_CleanFuseBlockVol(ComputeContext& cptCtx)
{
    if (!_cptPSOresetFuseBlockVol.GetPipelineStateObject())
        _CreateCptPSO_ResetFuseBlockVol();
    cptCtx.SetPipelineState(_cptPSOresetFuseBlockVol);
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
    if (!_cptPSOresetRenderBlockVol.GetPipelineStateObject())
        _CreateCptPSO_ResetRenderBlockVol();
    cptCtx.SetPipelineState(_cptPSOresetRenderBlockVol);
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
        Trans(cptCtx, _sensorMatrixBuf, csSRV);
        {
            GPU_PROFILE(cptCtx, L"Occupied2UpdateQ");
            if (!_cptPSOupdateQFromOccupiedQ.GetPipelineStateObject())
                _CreateCptPSO_UpdateQFromOccupiedQ();
            cptCtx.SetPipelineState(_cptPSOupdateQFromOccupiedQ);
            cptCtx.ResetCounter(_updateBlocksBuf);
            std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numUAVs> UAVs = _nullUAVs;
            UAVs[0] = _updateBlocksBuf.GetUAV();
            UAVs[1] = _fuseBlockVol.GetUAV();
            std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numSRVs> SRVs = _nullSRVs;
            SRVs[0] = _sensorMatrixBuf.GetSRV();
            SRVs[1] = _occupiedBlocksBuf.GetSRV();
            SRVs[2] = _occupiedBlocksBuf.GetCounterSRV(cptCtx);
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
            if (!_cptPSOupdateQFromDepthMap.GetPipelineStateObject())
                _CreateCptPSO_UpdateQFromDepthMap();
            cptCtx.SetPipelineState(_cptPSOupdateQFromDepthMap);
            // UAV barrier for _updateBlocksBuf is omit since the order of ops
            // on it does not matter before reading it during UpdateVoxel
            std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numUAVs> UAVs = _nullUAVs;
            UAVs[0] = _updateBlocksBuf.GetUAV();
            UAVs[1] = _fuseBlockVol.GetUAV();
            std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numSRVs> SRVs = _nullSRVs;
            SRVs[0] = _sensorMatrixBuf.GetSRV();
            SRVs[1] = pDepthTex->GetSRV();
            SRVs[2] = pWeightTex->GetSRV();
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
            if (!_cptPSOupdateQResolve.GetPipelineStateObject())
                _CreateCptPSO_UpdateQResolve();
            cptCtx.SetPipelineState(_cptPSOupdateQResolve);
            std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numUAVs> UAVs = _nullUAVs;
            UAVs[0] = _indirectParams.GetUAV();
            std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numSRVs> SRVs = _nullSRVs;
            SRVs[0] = _updateBlocksBuf.GetCounterSRV(cptCtx);
            Bind(cptCtx, 2, 0, numUAVs, UAVs.data());
            Bind(cptCtx, 3, 0, numSRVs, SRVs.data());
            cptCtx.Dispatch1D(1, WARP_SIZE);
        }
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
            ComputePSO& PSO = _cptPSOblockUpdate[buf.type][_TGSize];
            if (!PSO.GetPipelineStateObject())
                _CreateCptPSO_BlockUpdate(buf.type,_TGSize);
            cptCtx.SetPipelineState(PSO);
            std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numUAVs> UAVs;
            UAVs[0] = buf.UAV[0];
            UAVs[1] = buf.UAV[1];
            UAVs[2] = _fuseBlockVol.GetUAV();
            UAVs[3] = _newFuseBlocksBuf.GetUAV();
            UAVs[4] = _freedFuseBlocksBuf.GetUAV();
            UAVs[5] = _occupiedBlocksBuf.GetUAV();
            UAVs[6] = _renderBlockVol.GetUAV();
            std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numSRVs> SRVs = _nullSRVs;
            SRVs[0] = _sensorMatrixBuf.GetSRV();
            SRVs[1] = pDepthTex->GetSRV();
            SRVs[2] = buf.SRV[0];
            SRVs[3] = buf.SRV[1];
            SRVs[4] = pWeightTex->GetSRV();
            SRVs[5] = _updateBlocksBuf.GetSRV();
            Bind(cptCtx, 2, 0, numUAVs, UAVs.data());
            Bind(cptCtx, 3, 0, numSRVs, SRVs.data());
            cptCtx.DispatchIndirect(_indirectParams, 12);
        }
        BeginTrans(cptCtx, _fuseBlockVol, UAV);
        BeginTrans(cptCtx, _occupiedBlocksBuf, UAV);
        BeginTrans(cptCtx, _newFuseBlocksBuf, csSRV);
        BeginTrans(cptCtx, _freedFuseBlocksBuf, csSRV);
        // Read queue buffer counter back for debugging
        if (_readBackGPUBufStatus) {
            Graphics::g_cmdListMngr.WaitForFence(_readBackFence);
            static D3D12_RANGE range = {0, 16};
            static D3D12_RANGE umapRange = {};
            _debugBuf.Map(&range, reinterpret_cast<void**>(&_readBackPtr));
            memcpy(_readBackData, _readBackPtr, 4 * sizeof(uint32_t));
            _debugBuf.Unmap(&umapRange);
            cptCtx.CopyBufferRegion(_debugBuf, 0,
                _occupiedBlocksBuf.GetCounterBuffer(), 0, 4);
            cptCtx.CopyBufferRegion(_debugBuf, 4,
                _updateBlocksBuf.GetCounterBuffer(), 0, 4);
            cptCtx.CopyBufferRegion(_debugBuf, 8,
                _newFuseBlocksBuf.GetCounterBuffer(), 0, 4);
            cptCtx.CopyBufferRegion(_debugBuf, 12,
                _freedFuseBlocksBuf.GetCounterBuffer(), 0, 4);
            _readBackFence = cptCtx.Flush();
        }
        // Create indirect argument and params for Filling in
        // OccupiedBlockFreedSlot and AppendingNewOccupiedBlock
        Trans(cptCtx, _indirectParams, UAV);
        Trans(cptCtx, _jobParamBuf, UAV);
        {
            GPU_PROFILE(cptCtx, L"OccupiedQPrepare");
            if (!_cptPSOprepareNewFreeQ.GetPipelineStateObject())
                _CreateCptPSO_PrepareNewFreeQ();
            cptCtx.SetPipelineState(_cptPSOprepareNewFreeQ);
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
            if (!_cptPSOfillFreeQ.GetPipelineStateObject())
                _CreateCptPSO_FillFreeQ();
            cptCtx.SetPipelineState(_cptPSOfillFreeQ);
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
            if (!_cptPSOappendOccupiedQ.GetPipelineStateObject())
                _CreateCptPSO_AppendOccupiedQ();
            cptCtx.SetPipelineState(_cptPSOappendOccupiedQ);
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
            if (!_cptPSOoccupiedQResolve.GetPipelineStateObject())
                _CreateCptPSO_OccupiedQResolve();
            cptCtx.SetPipelineState(_cptPSOoccupiedQResolve);
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
        ComputePSO& PSO = _cptPSOvoxelUpdate[buf.type][type];
        if (!PSO.GetPipelineStateObject())
            _CreateCptPSO_VoxelUpdate(buf.type,type);
        cptCtx.SetPipelineState(PSO);

        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numUAVs> UAVs = _nullUAVs;
        UAVs[0] = buf.UAV[0];
        UAVs[1] = buf.UAV[1];
        UAVs[2] = _renderBlockVol.GetUAV();
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numSRVs> SRVs = _nullSRVs;
        SRVs[0] = _sensorMatrixBuf.GetSRV();
        SRVs[1] = pDepthTex->GetSRV();
        SRVs[2] = buf.SRV[0];
        SRVs[3] = buf.SRV[1];
        SRVs[4] = pWeightTex->GetSRV();
        Bind(cptCtx, 2, 0, numUAVs, UAVs.data());
        Bind(cptCtx, 3, 0, numSRVs, SRVs.data());
        cptCtx.Dispatch3D(xyz.x, xyz.y, xyz.z, THREAD_X, THREAD_Y, THREAD_Z);
    }
}

void
TSDFVolume::_RenderNearFar(GraphicsContext& gfxCtx, const CamType& cam)
{
    if (!_gfxPSORayNearFar[_vsMode][cam].GetPipelineStateObject())
        _CreateGfxPSO_RayNearFar(_vsMode, cam);
    gfxCtx.SetPipelineState(_gfxPSORayNearFar[_vsMode][cam]);
    gfxCtx.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numUAVs> UAVs = _nullUAVs;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numSRVs> SRVs = _nullSRVs;
    SRVs[0] = _sensorMatrixBuf.GetSRV();
    SRVs[1] = _renderBlockVol.GetSRV();
    Bind(gfxCtx, 2, 0, numUAVs, UAVs.data());
    Bind(gfxCtx, 3, 0, numSRVs, SRVs.data());

    const uint3 xyz = _volParam->u3VoxelReso;
    const uint ratio = _volParam->uVoxelRenderBlockRatio;
    uint BrickCount = xyz.x * xyz.y * xyz.z / ratio / ratio / ratio;
    if (_vsMode == kIndexed) {
        gfxCtx.Draw(BrickCount * 16);
    } else {
        gfxCtx.SetIndexBuffer(_cubeTriangleStripIB.IndexBufferView());
        gfxCtx.DrawIndexedInstanced(
            CUBE_TRIANGLESTRIP_LENGTH, BrickCount, 0, 0, 0);
    }
}

void
TSDFVolume::_RenderVolume(GraphicsContext& gfxCtx,
    const ManagedBuf::BufInterface& buf, const TSDFVolume::CamType& cam)
{
    VolumeStruct type = _useStepInfoTex ? kBlockVol : kVoxel;
    GraphicsPSO& PSO = _gfxPSORaycast[buf.type][type][_filterType][cam];
    if (!PSO.GetPipelineStateObject())
        _CreateGfxPSO_RayCast(cam, buf.type, type, _filterType);
    gfxCtx.SetPipelineState(PSO);
    gfxCtx.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numUAVs> UAVs = _nullUAVs;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numSRVs> SRVs = _nullSRVs;
    SRVs[0] = _sensorMatrixBuf.GetSRV();
    SRVs[1] = buf.SRV[0];
    if (_useStepInfoTex) {
        SRVs[2] = _nearFarTex[cam].GetSRV();
        SRVs[3] = _renderBlockVol.GetSRV();
    }
    Bind(gfxCtx, 2, 0, numUAVs, UAVs.data());
    Bind(gfxCtx, 3, 0, numSRVs, SRVs.data());
    gfxCtx.Draw(3);
}

void
TSDFVolume::_RenderHelperWireframe(GraphicsContext& gfxCtx)
{
    if (!_gfxPSOHelperWireframe.GetPipelineStateObject())
        _CreateGfxPSO_HelperWireframe();
    gfxCtx.SetPipelineState(_gfxPSOHelperWireframe);
    Trans(gfxCtx, _sensorMatrixBuf, vsSRV);
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numUAVs> UAVs = _nullUAVs;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numSRVs> SRVs = _nullSRVs;
    SRVs[0] = _sensorMatrixBuf.GetSRV();
    Bind(gfxCtx, 2, 0, numUAVs, UAVs.data());
    Bind(gfxCtx, 3, 0, numSRVs, SRVs.data());
    gfxCtx.DrawIndexedInstanced(CUBE_LINESTRIP_LENGTH, 3, 0, 0, 0);
}

void
TSDFVolume::_RenderBrickGrid(GraphicsContext& gfxCtx)
{
    if (!_gfxPSODebugRayNearFar.GetPipelineStateObject())
        _CreateGfxPSO_DebugRayNearFar();
    gfxCtx.SetPipelineState(_gfxPSODebugRayNearFar);
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numUAVs> UAVs = _nullUAVs;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, numSRVs> SRVs = _nullSRVs;
    SRVs[1] = _renderBlockVol.GetSRV();
    Bind(gfxCtx, 2, 0, numUAVs, UAVs.data());
    Bind(gfxCtx, 3, 0, numSRVs, SRVs.data());
    const uint3 xyz = _volParam->u3VoxelReso;
    const uint ratio = _volParam->uVoxelRenderBlockRatio;
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