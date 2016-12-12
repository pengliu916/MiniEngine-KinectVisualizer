#include "pch.h"
#include "TSDFVolume.h"

using namespace DirectX;
using namespace Microsoft::WRL;
using namespace std;

#define frand() ((float)rand()/RAND_MAX)
#define Bind(ctx, rootIdx, offset, count, resource) \
ctx.SetDynamicDescriptors(rootIdx, offset, count, resource)

#define BindCB(ctx, rootIdx, size, ptr) \
ctx.SetDynamicConstantBufferView(rootIdx, size, ptr)

namespace {
    const D3D12_RESOURCE_STATES UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    const D3D12_RESOURCE_STATES psSRV =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    const D3D12_RESOURCE_STATES  csSRV =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const D3D12_RESOURCE_STATES  vsSRV =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const D3D12_RESOURCE_STATES RTV = D3D12_RESOURCE_STATE_RENDER_TARGET;
    const D3D12_RESOURCE_STATES DSV = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    const D3D12_RESOURCE_STATES IARG = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;


    const UINT kBuf = ManagedBuf::kNumType;
    const UINT kStruct = TSDFVolume::kNumStruct;
    const UINT kFilter = TSDFVolume::kNumFilter;
    const UINT kTG = TSDFVolume::kTG;

    const DXGI_FORMAT _stepInfoTexFormat = DXGI_FORMAT_R16G16_FLOAT;
    bool _typedLoadSupported = false;

    bool _useStepInfoTex = true;
    bool _stepInfoDebug = false;
    bool _blockVolumeUpdate = true;
    bool _readBackGPUBufStatus = true;
    bool _noInstance = false;

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
    // PSOs for rendering
    GraphicsPSO _gfxVCamRenderPSO[kBuf][kStruct][kFilter];
    GraphicsPSO _gfxSensorRenderPSO[kBuf][kStruct][kFilter];
    GraphicsPSO _gfxStepInfoVCamPSO[2]; // index is noInstance on/off
    GraphicsPSO _gfxStepInfoSensorPSO[2]; // index is noInstance on/off
    GraphicsPSO _gfxStepInfoDebugPSO;
    // PSOs for reseting
    ComputePSO _cptFuseBlockVolResetPSO;
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
        int iLen = (int)wcslen(shaderName);
        char target[8];
        wchar_t fileName[128];
        swprintf_s(fileName, 128, L"TSDFVolume_%ls.hlsl", shaderName);
        switch (shaderName[iLen-2])
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
            D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0, 7);
        _rootsig[3].InitAsDescriptorRange(
            D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 4);
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
        DXGI_FORMAT ColorFormat = Graphics::g_SceneColorBuffer.GetFormat();
        DXGI_FORMAT DepthFormat = Graphics::g_SceneDepthBuffer.GetFormat();
        DXGI_FORMAT ExtractTexFormat = ColorFormat;
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

        compiledOnce = false;
        for (int k = 0; k < kStruct; ++k) {
            for (int i = 0; i < kBuf; ++i) {
                for (int j = 0; j < kFilter; ++j) {
                    _gfxVCamRenderPSO[i][k][j].SetRootSignature(_rootsig);
                    _gfxVCamRenderPSO[i][k][j].SetInputLayout(
                        _countof(inputElementDescs), inputElementDescs);
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
                        1, &ColorFormat, DepthFormat);
                    _gfxVCamRenderPSO[i][k][j].SetPixelShader(
                        raycastVCamPS[i][k][j]->GetBufferPointer(),
                        raycastVCamPS[i][k][j]->GetBufferSize());
                    _gfxSensorRenderPSO[i][k][j] = _gfxVCamRenderPSO[i][k][j];
                    _gfxSensorRenderPSO[i][k][j].SetVertexShader(
                        quadSensorVS->GetBufferPointer(),
                        quadSensorVS->GetBufferSize());
                    _gfxSensorRenderPSO[i][k][j].SetRenderTargetFormats(
                        1, &ExtractTexFormat, DXGI_FORMAT_UNKNOWN);
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
        ComPtr<ID3DBlob> stepInfoPS, stepInfoDebugPS;
        ComPtr<ID3DBlob> stepInfoVCamVS[2], stepInfoSensorVS[2];
        D3D_SHADER_MACRO macro1[] = {
            {"__hlsl", "1"},
            {"DEBUG_VIEW", "0"},
            {"FOR_VCAMERA", "1"},
            {"FOR_SENSOR", "0"},
            {nullptr, nullptr}
        };
        V(_Compile(L"StepInfo_ps", macro1, &stepInfoPS));
        V(_Compile(L"StepInfo_vs", macro1, &stepInfoVCamVS[0]));
        V(_Compile(L"StepInfoNoInstance_vs", macro1, &stepInfoVCamVS[1]));
        macro1[2].Definition = "0"; macro1[3].Definition = "1";
        V(_Compile(L"StepInfo_vs", macro1, &stepInfoSensorVS[0]));
        V(_Compile(L"StepInfoNoInstance_vs", macro1, &stepInfoSensorVS[1]));
        macro[1].Definition = "1";
        V(_Compile(L"StepInfo_ps", macro1, &stepInfoDebugPS));

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
            1, &ColorFormat, DepthFormat);
        _gfxStepInfoVCamPSO[0].SetRenderTargetFormats(
            1, &_stepInfoTexFormat, DXGI_FORMAT_UNKNOWN);
        _gfxStepInfoVCamPSO[0].SetPixelShader(
            stepInfoPS->GetBufferPointer(), stepInfoPS->GetBufferSize());
        _gfxStepInfoDebugPSO.SetPixelShader(
            stepInfoPS->GetBufferPointer(), stepInfoDebugPS->GetBufferSize());
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
    : _volBuf(XMUINT3(512, 512, 512)),
    _nearFarTex(XMVectorSet(MAX_DEPTH, 0, 0, 0))
{
    _volParam = &_cbPerCall.vParam;
    _volParam->fMaxWeight = 20.f;
    _volParam->fVoxelSize = 1.f / 100.f;
    _cbPerCall.f2DepthRange = float2(0.2f, 12.f);
}

TSDFVolume::~TSDFVolume()
{
}

void
TSDFVolume::OnCreateResource(const int2& depthReso, const int2& colorReso)
{
    ASSERT(Graphics::g_device);
    // Create resource for volume
    _volBuf.CreateResource();

    // Create resource for job count for OccupiedBlockUpdate pass
    _jobParamBuf.Create(L"JobCountBuf", 4, 4);

    // Create debug buffer for all queue buffer size. 
    // 0 : _occupiedBlocksBuf.counter
    // 1 : _updateBlocksBuf.counter
    // 2 : _newOccupiedBlocksBuf.counter
    // 3 : _freedOccupiedBlocksBuf.counter
    _debugBuf.Create(L"DebugBuf", 4, 4);

    // Initial value for dispatch indirect args. args are thread group count
    // x, y, z. Since we only need 1 dimensional dispatch thread group, we
    // pre-populate 1, 1 for threadgroup Ys and Zs
    __declspec(align(16)) const uint32_t initArgs[12] = {
        0, 1, 1, // for VolumeUpdate_Pass2, X = OccupiedBlocks / WARP_SIZE
        0, 1, 1, // for VolumeUpdate_Pass3, X = numUpdateBlock * TG/Block
        0, 1, 1, // for OccupiedBlockUpdate_Pass2, X = numFreeSlots / WARP_SIZE
        0, 1, 1, // for OccupiedBlockUpdate_Pass3, X = numLeftOver / WARP_SIZE
    };
    _indirectParams.Create(L"TSDFVolume Indirect Params",
        1, 4 * sizeof(D3D12_DISPATCH_ARGUMENTS), initArgs);

    std::call_once(_psoCompiled_flag, _CreateResource);

    _cbPerCall.i2ColorReso = colorReso;
    _cbPerCall.i2DepthReso = depthReso;

    _depthViewPort.Width = static_cast<float>(depthReso.x);
    _depthViewPort.Height = static_cast<float>(depthReso.y);
    _depthViewPort.MaxDepth = 1.f;

    _depthSissorRect.right = static_cast<LONG>(depthReso.x);
    _depthSissorRect.bottom = static_cast<LONG>(depthReso.y);

    _extractedDepth.Create(L"ExtracedDepth",
        depthReso.x, depthReso.y, 1, Graphics::g_SceneColorBuffer.GetFormat());

    const uint3 reso = _volBuf.GetReso();
    _submittedReso = reso;
    _UpdateVolumeSettings(reso, _volParam->fVoxelSize);
    _UpdateBlockSettings(_fuseBlockVoxelRatio, _renderBlockVoxelRatio, _TGSize);

    // Create Spacial Structure Buffer
    _CreateRenderBlockVol(reso, _renderBlockVoxelRatio);
    _CreateFuseBlockVolAndRelatedBuf(reso, _fuseBlockVoxelRatio);
}

void
TSDFVolume::OnDestory()
{
    _extractedDepth.Destroy();
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
    _nearFarTex.Destroy();
    _cubeVB.Destroy();
    _cubeTriangleStripIB.Destroy();
    _cubeLineStripIB.Destroy();
}

void
TSDFVolume::OnResize()
{
    int32_t width = Core::g_config.swapChainDesc.Width;
    int32_t height = Core::g_config.swapChainDesc.Height;

    float fAspectRatio = width / (FLOAT)height;
    _cbPerCall.fWideHeightRatio = fAspectRatio;
    _cbPerCall.fClipDist = 0.1f;
    float fHFov = XM_PIDIV4;
    _cbPerCall.fTanHFov = tan(fHFov / 2.f);

    _nearFarTex.Destroy();
    // Create MinMax Buffer but make sure it's at least is the sensor reso
    width = max(width, _cbPerCall.i2DepthReso.x);
    height = max(height, _cbPerCall.i2DepthReso.y);
    _nearFarTex.Create(L"StepInfoTex", width, height, 0, _stepInfoTexFormat);
}

void
TSDFVolume::OnReset()
{
    ComputeContext& cptCtx = ComputeContext::Begin(L"ResetContext");
    cptCtx.SetRootSignature(_rootsig);
    BindCB(cptCtx, 0, sizeof(_cbPerFrame), (void*)&_cbPerFrame);
    BindCB(cptCtx, 1, sizeof(_cbPerCall), (void*)&_cbPerCall);
    _CleanTSDFVols(cptCtx, _curBufInterface);
    _CleanFuseBlockVol(cptCtx);
    _CleanRenderBlockVol(cptCtx);
    _ClearBlockQueues(cptCtx);
    cptCtx.Finish(true);
}

void
TSDFVolume::OnUpdate()
{
    ManagedBuf::BufInterface newBufInterface = _volBuf.GetResource();
    _curBufInterface = newBufInterface;

    const uint3& reso = _volBuf.GetReso();
    if (_IsResolutionChanged(reso, _curReso)) {
        _curReso = reso;
        _UpdateVolumeSettings(reso, _volParam->fVoxelSize);
        _CreateFuseBlockVolAndRelatedBuf(reso, _fuseBlockVoxelRatio);
    }
}

void
TSDFVolume::OnIntegrate(CommandContext& cmdCtx, ColorBuffer* pDepthTex,
    ColorBuffer* pColorTex, const DirectX::XMMATRIX& mDepth_T)
{
    ComputeContext& cptCtx = cmdCtx.GetComputeContext();
    
    cptCtx.SetRootSignature(_rootsig);
    _cbPerFrame.mDepthView = XMMatrixInverse(nullptr, mDepth_T);
    _cbPerFrame.mDepthViewInv = mDepth_T;
    BindCB(cptCtx, 0, sizeof(_cbPerFrame), (void*)&_cbPerFrame);
    BindCB(cptCtx, 1, sizeof(_cbPerCall), (void*)&_cbPerCall);
    cmdCtx.TransitionResource(_renderBlockVol, UAV);
    cmdCtx.TransitionResource(*_curBufInterface.resource[0], UAV);
    cmdCtx.TransitionResource(*_curBufInterface.resource[1], UAV);
    _UpdateVolume(cmdCtx, _curBufInterface, pDepthTex, pColorTex);
    cmdCtx.BeginResourceTransition(*_curBufInterface.resource[0], psSRV);
    if (_useStepInfoTex) {
        cmdCtx.BeginResourceTransition(_renderBlockVol, vsSRV | psSRV);
    }
}

void
TSDFVolume::OnRender(CommandContext& cmdCtx,
    const DirectX::XMMATRIX& mProj_T, const DirectX::XMMATRIX& mView_T)
{
    _UpdateRenderCamData(mProj_T, mView_T);

    cmdCtx.BeginResourceTransition(Graphics::g_SceneColorBuffer, RTV);
    cmdCtx.BeginResourceTransition(Graphics::g_SceneDepthBuffer, DSV);

    GraphicsContext& gfxCtx = cmdCtx.GetGraphicsContext();
    gfxCtx.TransitionResource(Graphics::g_SceneColorBuffer, RTV);
    gfxCtx.TransitionResource(
        Graphics::g_SceneDepthBuffer, DSV, !_useStepInfoTex);

    if (_useStepInfoTex) {
        gfxCtx.TransitionResource(_nearFarTex, RTV, true);
        gfxCtx.ClearColor(_nearFarTex);
    }

    gfxCtx.ClearColor(Graphics::g_SceneColorBuffer);
    gfxCtx.ClearDepth(Graphics::g_SceneDepthBuffer);

    gfxCtx.SetRootSignature(_rootsig);
    BindCB(gfxCtx, 0, sizeof(_cbPerFrame), (void*)&_cbPerFrame);
    BindCB(gfxCtx, 1, sizeof(_cbPerCall), (void*)&_cbPerCall);
    gfxCtx.SetViewport(Graphics::g_DisplayPlaneViewPort);
    gfxCtx.SetScisor(Graphics::g_DisplayPlaneScissorRect);
    gfxCtx.SetVertexBuffer(0, _cubeVB.VertexBufferView());

    if (_useStepInfoTex) {
        GPU_PROFILE(gfxCtx, L"NearFar_Render");
        gfxCtx.TransitionResource(_renderBlockVol, vsSRV | psSRV);
        _RenderNearFar(gfxCtx);
        gfxCtx.TransitionResource(_nearFarTex, psSRV);
    }
    {
        GPU_PROFILE(gfxCtx, L"Volume_Render");
        gfxCtx.TransitionResource(*_curBufInterface.resource[0], psSRV);
        _RenderVolume(gfxCtx, _curBufInterface);
        gfxCtx.BeginResourceTransition(*_curBufInterface.resource[0], UAV);
    }
    if (_useStepInfoTex) {
        if (_stepInfoDebug) {
            GPU_PROFILE(gfxCtx, L"DebugGrid_Render");
            _RenderBrickGrid(gfxCtx);
        }
        cmdCtx.BeginResourceTransition(_nearFarTex, RTV);
    }
}

void
TSDFVolume::OnExtractSurface(CommandContext& cmdCtx,
    const DirectX::XMMATRIX& mSensor_T)
{
    _UpdateSensorData(mSensor_T);
    GraphicsContext& gfxCtx = cmdCtx.GetGraphicsContext();
    cmdCtx.TransitionResource(_extractedDepth, RTV);
    if (_useStepInfoTex) {
        gfxCtx.TransitionResource(_nearFarTex, RTV, true);
        gfxCtx.ClearColor(_nearFarTex);
    }

    gfxCtx.ClearColor(_extractedDepth);

    gfxCtx.SetRootSignature(_rootsig);
    BindCB(gfxCtx, 0, sizeof(_cbPerFrame), (void*)&_cbPerFrame);
    BindCB(gfxCtx, 1, sizeof(_cbPerCall), (void*)&_cbPerCall);
    gfxCtx.SetViewport(_depthViewPort);
    gfxCtx.SetScisor(_depthSissorRect);
    gfxCtx.SetVertexBuffer(0, _cubeVB.VertexBufferView());

    if (_useStepInfoTex) {
        GPU_PROFILE(gfxCtx, L"NearFar_Extract");
        gfxCtx.TransitionResource(_renderBlockVol, vsSRV | psSRV);
        _RenderNearFar(gfxCtx, true);
        gfxCtx.TransitionResource(_nearFarTex, psSRV);
    }
    {
        GPU_PROFILE(gfxCtx, L"Volume_Extract");
        gfxCtx.TransitionResource(*_curBufInterface.resource[0], psSRV);
        _RenderVolume(gfxCtx, _curBufInterface, true);
        gfxCtx.BeginResourceTransition(*_curBufInterface.resource[0], UAV);
    }
    if (_useStepInfoTex) {
        cmdCtx.BeginResourceTransition(_nearFarTex, RTV);
    }
    _extractedDepth.GuiShow();
}

void
TSDFVolume::RenderGui()
{
    static bool showPenal = true;
    if (!ImGui::CollapsingHeader("Sparse Volume", 0, true, true)) {
        return;
    }
    ImGui::Checkbox("Block Volume Update", &_blockVolumeUpdate);
    ImGui::SameLine();
    ImGui::Checkbox("Debug", &_readBackGPUBufStatus);
    if (_readBackGPUBufStatus && _blockVolumeUpdate) {
        char buf[64];
        float occupiedBlockPct =
            (float)_readBackData[0] / _occupiedBlockQueueSize;
        sprintf_s(buf, 64, "%d/%d OccupiedBlocks",
            _readBackData[0], _occupiedBlockQueueSize);
        ImGui::ProgressBar(occupiedBlockPct, ImVec2(-1.f, 0.f), buf);
        float updateBlockPct =
            (float)_readBackData[1] / _updateBlockQueueSize;
        sprintf_s(buf, 64, "%d/%d UpdateBlocks",
            _readBackData[1], _updateBlockQueueSize);
        ImGui::ProgressBar(updateBlockPct, ImVec2(-1.f, 0.f), buf);
        float newQueuePct =
            (float)_readBackData[2] / _newOccupiedBlocksSize;
        sprintf_s(buf, 64, "%d/%d NewQueueBlocks",
            _readBackData[2], _newOccupiedBlocksSize);
        ImGui::ProgressBar(newQueuePct, ImVec2(-1.f, 0.f), buf);
        float freeQueuePct =
            (float)_readBackData[3] / _freedOccupiedBlocksSize;
        sprintf_s(buf, 64, "%d/%d FreeQueueBlocks",
            _readBackData[3], _freedOccupiedBlocksSize);
        ImGui::ProgressBar(freeQueuePct, ImVec2(-1.f, 0.f), buf);
    }
    ImGui::Text("BlockUpdate CS Threadgroup Size:");
    static int iTGSize = (int)_TGSize;
    ImGui::RadioButton("4x4x4##TG", &iTGSize, k64);
    ImGui::RadioButton("8x8x8##TG", &iTGSize, k512);
    if (iTGSize != (int)_TGSize) {
        if (iTGSize == (int)k512 && _fuseBlockVoxelRatio == 4) {
            iTGSize = (int)_TGSize;
        } else {
            _TGSize = (ThreadGroup)iTGSize;
            _UpdateBlockSettings(_fuseBlockVoxelRatio,
                _renderBlockVoxelRatio, _TGSize);
            _CreateFuseBlockVolAndRelatedBuf(_curReso, _fuseBlockVoxelRatio);
        }
    }
    ImGui::Separator();
    ImGui::Checkbox("StepInfoTex", &_useStepInfoTex);
    ImGui::Checkbox("NoInstance", &_noInstance);
    ImGui::SameLine();
    ImGui::Checkbox("DebugGrid", &_stepInfoDebug);
    ImGui::Separator();
    static int iFilterType = (int)_filterType;
    ImGui::RadioButton("No Filter", &iFilterType, kNoFilter);
    ImGui::RadioButton("Linear Filter", &iFilterType, kLinearFilter);
    ImGui::RadioButton("Linear Sampler", &iFilterType, kSamplerLinear);
    ImGui::RadioButton("Aniso Sampler", &iFilterType, kSamplerAniso);
    if (_volBuf.GetType() != ManagedBuf::k3DTexBuffer &&
        iFilterType > kLinearFilter) {
        iFilterType = _filterType;
    }
    _filterType = (FilterType)iFilterType;

    ImGui::Separator();
    ImGui::Text("Buffer Settings:");
    static int uBufferBitChoice = _volBuf.GetBit();
    static int uBufferTypeChoice = _volBuf.GetType();
    ImGui::RadioButton("8Bit", &uBufferBitChoice,
        ManagedBuf::k8Bit); ImGui::SameLine();
    ImGui::RadioButton("16Bit", &uBufferBitChoice,
        ManagedBuf::k16Bit); ImGui::SameLine();
    ImGui::RadioButton("32Bit", &uBufferBitChoice,
        ManagedBuf::k32Bit);
    ImGui::RadioButton("Use Typed Buffer", &uBufferTypeChoice,
        ManagedBuf::kTypedBuffer);
    ImGui::RadioButton("Use Texture3D Buffer", &uBufferTypeChoice,
        ManagedBuf::k3DTexBuffer);
    if (iFilterType > kLinearFilter &&
        uBufferTypeChoice != ManagedBuf::k3DTexBuffer) {
        uBufferTypeChoice = _volBuf.GetType();
    }
    if ((uBufferTypeChoice != _volBuf.GetType() ||
        uBufferBitChoice != _volBuf.GetBit()) &&
        !_volBuf.ChangeResource(_volBuf.GetReso(),
        (ManagedBuf::Type)uBufferTypeChoice,
            (ManagedBuf::Bit)uBufferBitChoice)) {
        uBufferTypeChoice = _volBuf.GetType();
    }

    ImGui::Separator();
    ImGui::Columns(2, "Block Ratio");
    ImGui::Text("FuseBlockRatio:"); ImGui::NextColumn();
    ImGui::Text("RenderBlockRatio:"); ImGui::NextColumn();
    static int fuseBlockRatio = (int)_fuseBlockVoxelRatio;
    static int renderBlockRatio = (int)_renderBlockVoxelRatio;
    ImGui::RadioButton("4x4x4##Fuse", &fuseBlockRatio, 4);
    ImGui::NextColumn();
    ImGui::RadioButton("4x4x4##Render", &renderBlockRatio, 4);
    ImGui::NextColumn();
    ImGui::RadioButton("8x8x8##Fuse", &fuseBlockRatio, 8);
    ImGui::NextColumn();
    ImGui::RadioButton("8x8x8##Render", &renderBlockRatio, 8);
    ImGui::NextColumn();
    ImGui::RadioButton("16x16x16##Fuse", &fuseBlockRatio, 16);
    ImGui::NextColumn();
    ImGui::RadioButton("16x16x16##Render", &renderBlockRatio, 16);
    ImGui::NextColumn(); ImGui::NextColumn();
    ImGui::RadioButton("32x32x32##Render", &renderBlockRatio, 32);

    if (fuseBlockRatio != _fuseBlockVoxelRatio) {
        if (!(fuseBlockRatio == 4 && _TGSize != k64) &&
            fuseBlockRatio <= renderBlockRatio) {
            _fuseBlockVoxelRatio = fuseBlockRatio;
            _UpdateBlockSettings(_fuseBlockVoxelRatio,
                _renderBlockVoxelRatio, _TGSize);
            _CreateFuseBlockVolAndRelatedBuf(
                _curReso, _fuseBlockVoxelRatio);
        } else {
            fuseBlockRatio = _fuseBlockVoxelRatio;
        }
    }
    if (renderBlockRatio != _renderBlockVoxelRatio) {
        if (renderBlockRatio >= fuseBlockRatio) {
            _renderBlockVoxelRatio = renderBlockRatio;
            _UpdateBlockSettings(_fuseBlockVoxelRatio,
                _renderBlockVoxelRatio, _TGSize);
            _CreateRenderBlockVol(
                _volBuf.GetReso(), _renderBlockVoxelRatio);
            _CreateFuseBlockVolAndRelatedBuf(
                _curReso, _fuseBlockVoxelRatio);
        } else {
            renderBlockRatio = _renderBlockVoxelRatio;
        }
    }
    ImGui::Columns(1);
    ImGui::Separator();

    ImGui::Text("Volume Size Settings:");
    static uint3 uiReso = _volBuf.GetReso();
    ImGui::AlignFirstTextHeightToWidgets();
    ImGui::Text("X:"); ImGui::SameLine();
    ImGui::RadioButton("128##X", (int*)&uiReso.x, 128); ImGui::SameLine();
    ImGui::RadioButton("256##X", (int*)&uiReso.x, 256); ImGui::SameLine();
    ImGui::RadioButton("384##X", (int*)&uiReso.x, 384); ImGui::SameLine();
    ImGui::RadioButton("512##X", (int*)&uiReso.x, 512);

    ImGui::AlignFirstTextHeightToWidgets();
    ImGui::Text("Y:"); ImGui::SameLine();
    ImGui::RadioButton("128##Y", (int*)&uiReso.y, 128); ImGui::SameLine();
    ImGui::RadioButton("256##Y", (int*)&uiReso.y, 256); ImGui::SameLine();
    ImGui::RadioButton("384##Y", (int*)&uiReso.y, 384); ImGui::SameLine();
    ImGui::RadioButton("512##Y", (int*)&uiReso.y, 512);

    ImGui::AlignFirstTextHeightToWidgets();
    ImGui::Text("Z:"); ImGui::SameLine();
    ImGui::RadioButton("128##Z", (int*)&uiReso.z, 128); ImGui::SameLine();
    ImGui::RadioButton("256##Z", (int*)&uiReso.z, 256); ImGui::SameLine();
    ImGui::RadioButton("384##Z", (int*)&uiReso.z, 384); ImGui::SameLine();
    ImGui::RadioButton("512##Z", (int*)&uiReso.z, 512);
    if ((_IsResolutionChanged(uiReso, _submittedReso) ||
        _volBuf.GetType() != uBufferTypeChoice) &&
        _volBuf.ChangeResource(
            uiReso, _volBuf.GetType(), ManagedBuf::k32Bit)) {
        PRINTINFO("Reso:%dx%dx%d", uiReso.x, uiReso.y, uiReso.z);
        _submittedReso = uiReso;
    } else {
        uiReso = _submittedReso;
    }
    ImGui::SliderFloat("MaxWeight", &_volParam->fMaxWeight, 1.f, 500.f);
    static float fVoxelSize = _volParam->fVoxelSize;
    if (ImGui::SliderFloat("VoxelSize",
        &fVoxelSize, 1.f / 256.f, 10.f / 256.f)) {
        _UpdateVolumeSettings(_curReso, fVoxelSize);
        _UpdateBlockSettings(_fuseBlockVoxelRatio,
            _renderBlockVoxelRatio, _TGSize);
    }

    ImGui::Separator();
    if (ImGui::Button("Recompile All Shaders")) {
        _CreatePSOs();
    }
    ImGui::Separator();
    if (ImGui::Button("Reset Volume")) {
        OnReset();
    }
}

ColorBuffer*
TSDFVolume::GetOutTex()
{
    return &_extractedDepth;
}

void
TSDFVolume::_CreateFuseBlockVolAndRelatedBuf(
    const uint3& reso, const uint ratio)
{
    Graphics::g_cmdListMngr.IdleGPU();
    uint32_t blockCount = (uint32_t)(reso.x * reso.y * reso.z / pow(ratio, 3));
    _fuseBlockVol.Destroy();
    _fuseBlockVol.Create(L"BlockVol", reso.x / ratio, reso.y / ratio,
        reso.z / ratio, 1, DXGI_FORMAT_R32_UINT);
    ComputeContext& cptCtx = ComputeContext::Begin(L"ResetBlockVol");
    _CleanFuseBlockVol(cptCtx, true);
    _CleanRenderBlockVol(cptCtx);
    cptCtx.Finish(true);
    
    _updateBlocksBuf.Destroy();
    _updateBlockQueueSize = blockCount;
    _updateBlocksBuf.Create(L"UpdateBlockQueue", _updateBlockQueueSize, 4);
    
    _occupiedBlocksBuf.Destroy();
    _occupiedBlockQueueSize = blockCount;
    _occupiedBlocksBuf.Create(L"OccupiedBlockQueue", _updateBlockQueueSize, 4);
    
    _newFuseBlocksBuf.Destroy();
    _newOccupiedBlocksSize = (uint32_t)(_occupiedBlockQueueSize * 0.3f);
    _newFuseBlocksBuf.Create(L"NewOccupiedBlockBuf",
        _newOccupiedBlocksSize, 4);

    _freedFuseBlocksBuf.Destroy();
    _freedOccupiedBlocksSize = (uint32_t)(_occupiedBlockQueueSize * 0.3f);
    _freedFuseBlocksBuf.Create(L"FreedOccupiedBlockBuf",
        _freedOccupiedBlocksSize, 4);
}

void
TSDFVolume::_CreateRenderBlockVol(const uint3& reso, const uint ratio)
{
    Graphics::g_cmdListMngr.IdleGPU();
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
TSDFVolume::_UpdateSensorData(const DirectX::XMMATRIX& mSensorView_T)
{
    _cbPerFrame.mDepthView = mSensorView_T;
    _cbPerFrame.mDepthViewInv = XMMatrixInverse(nullptr, mSensorView_T);
}

void
TSDFVolume::_UpdateVolumeSettings(const uint3 reso, const float voxelSize)
{
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
}

void
TSDFVolume::_UpdateBlockSettings(const uint fuseBlockVoxelRatio,
    const uint renderBlockVoxelRatio, const TSDFVolume::ThreadGroup tg)
{
    _volParam->uVoxelFuseBlockRatio = fuseBlockVoxelRatio;
    _volParam->uVoxelRenderBlockRatio = renderBlockVoxelRatio;
    _volParam->fFuseBlockSize = fuseBlockVoxelRatio * _volParam->fVoxelSize;
    _volParam->fRenderBlockSize = renderBlockVoxelRatio * _volParam->fVoxelSize;
    uint threadCtr1D = 8;
    switch (tg) {
    case k512: threadCtr1D = 8; break;
    case k64: threadCtr1D = 4; break;
    }
    uint TGBlockRatio = fuseBlockVoxelRatio / threadCtr1D;
    _cbPerCall.uTGFuseBlockRatio = TGBlockRatio;
    _cbPerCall.uTGPerFuseBlock =
        TGBlockRatio * TGBlockRatio * TGBlockRatio;
}

void
TSDFVolume::_ClearBlockQueues(ComputeContext& cptCtx)
{
    cptCtx.ClearUAV(_occupiedBlocksBuf);
    cptCtx.ResetCounter(_occupiedBlocksBuf);
    cptCtx.ClearUAV(_updateBlocksBuf);
    cptCtx.ResetCounter(_updateBlocksBuf);
    cptCtx.ClearUAV(_newFuseBlocksBuf);
    cptCtx.ResetCounter(_newFuseBlocksBuf);
    cptCtx.ClearUAV(_freedFuseBlocksBuf);
    cptCtx.ResetCounter(_freedFuseBlocksBuf);
}

void
TSDFVolume::_CleanTSDFVols(ComputeContext& cptCtx,
    const ManagedBuf::BufInterface& buf, bool updateCB)
{
    cptCtx.SetPipelineState(_cptTSDFBufResetPSO[buf.type]);
    cptCtx.SetRootSignature(_rootsig);
    if (updateCB) {
        _UpdateConstantBuffer<ComputeContext>(cptCtx);
    }
    Bind(cptCtx, 2, 0, 2, buf.UAV);
    const uint3 xyz = _volParam->u3VoxelReso;
    cptCtx.Dispatch3D(xyz.x, xyz.y, xyz.z, THREAD_X, THREAD_Y, THREAD_Z);
}

void
TSDFVolume::_CleanFuseBlockVol(ComputeContext& cptCtx, bool updateCB)
{
    cptCtx.SetPipelineState(_cptFuseBlockVolResetPSO);
    cptCtx.SetRootSignature(_rootsig);
    if (updateCB) {
        _UpdateConstantBuffer<ComputeContext>(cptCtx);
    }
    Bind(cptCtx, 2, 1, 1, &_fuseBlockVol.GetUAV());
    const uint3 xyz = _volParam->u3VoxelReso;
    const uint ratio = _volParam->uVoxelFuseBlockRatio;
    cptCtx.Dispatch3D(xyz.x / ratio, xyz.y / ratio, xyz.z / ratio,
        THREAD_X, THREAD_Y, THREAD_Z);
}

void
TSDFVolume::_CleanRenderBlockVol(ComputeContext& cptCtx)
{
    cptCtx.SetPipelineState(_cptRenderBlockVolResetPSO);
    cptCtx.SetRootSignature(_rootsig);
    Bind(cptCtx, 2, 0, 1, &_renderBlockVol.GetUAV());
    const uint3 xyz = _volParam->u3VoxelReso;
    const uint ratio = _renderBlockVoxelRatio;
    cptCtx.Dispatch3D(xyz.x / ratio, xyz.y / ratio, xyz.z / ratio,
        THREAD_X, THREAD_Y, THREAD_Z);
}

void
TSDFVolume::_UpdateVolume(CommandContext& cmdCtx,
    const ManagedBuf::BufInterface& buf,
    ColorBuffer* pDepthTex, ColorBuffer* pColorTex)
{
    VolumeStruct type = _useStepInfoTex ? kBlockVol : kVoxel;
    const uint3 xyz = _volParam->u3VoxelReso;
    ComputeContext& cptCtx = cmdCtx.GetComputeContext();
    cptCtx.SetRootSignature(_rootsig);
    if (_blockVolumeUpdate) {
        // Add blocks to UpdateBlockQueue from OccupiedBlockQueue
        {
            GPU_PROFILE(cptCtx, L"Occupied2UpdateQ");
            cptCtx.ResetCounter(_updateBlocksBuf);
            cptCtx.TransitionResource(_updateBlocksBuf, UAV);
            cptCtx.TransitionResource(_fuseBlockVol, UAV);
            cptCtx.TransitionResource(_occupiedBlocksBuf, csSRV);
            cptCtx.TransitionResource(_indirectParams, IARG);
            cptCtx.SetPipelineState(_cptBlockVolUpdate_Pass1);
            Bind(cptCtx, 2, 0, 1, &_updateBlocksBuf.GetUAV());
            Bind(cptCtx, 2, 1, 1, &_fuseBlockVol.GetUAV());
            Bind(cptCtx, 3, 0, 1, &_occupiedBlocksBuf.GetSRV());
            Bind(cptCtx, 3, 1, 1, &_occupiedBlocksBuf.GetCounterSRV(cmdCtx));
            cptCtx.DispatchIndirect(_indirectParams, 0);
        }
        /*cptCtx.ResetCounter(_updateBlocksBuf);
        cptCtx.ResetCounter(_freedOccupiedBlocksBuf);
        cptCtx.ResetCounter(_newOccupiedBlocksBuf);
        cptCtx.ResetCounter(_occupiedBlocksBuf);
        _CleanBlockStateVol(cptCtx,true);*/
        // Add blocks to UpdateBlockQueue from DepthMap
        {
            GPU_PROFILE(cptCtx, L"Depth2UpdateQ");
            cptCtx.TransitionResource(_fuseBlockVol, UAV);
            cptCtx.SetPipelineState(_cptBlockVolUpdate_Pass2);
            // UAV barrier for _updateBlocksBuf is omit since the order of ops
            // on it does not matter before reading it during UpdateVoxel
            Bind(cptCtx, 2, 0, 1, &_updateBlocksBuf.GetUAV());
            Bind(cptCtx, 2, 1, 1, &_fuseBlockVol.GetUAV());
            Bind(cptCtx, 3, 0, 1, &pDepthTex->GetSRV());
            cptCtx.Dispatch2D(
                _cbPerCall.i2DepthReso.x, _cbPerCall.i2DepthReso.y);
        }
        // Resolve UpdateBlockQueue for DispatchIndirect
        {
            GPU_PROFILE(cptCtx, L"UpdateQResolve");
            cptCtx.TransitionResource(_indirectParams, UAV);
            cptCtx.SetPipelineState(_cptBlockVolUpdate_Resolve);
            Bind(cptCtx, 3, 0, 1, &_updateBlocksBuf.GetCounterSRV(cmdCtx));
            Bind(cptCtx, 2, 0, 1, &_indirectParams.GetUAV());
            cptCtx.Dispatch1D(1, WARP_SIZE);
        }
        // Update voxels in blocks from UpdateBlockQueue and create queues for
        // NewOccupiedBlocks and FreedOccupiedBlocks
        {
            GPU_PROFILE(cptCtx, L"UpdateVoxels");
            cptCtx.TransitionResource(_occupiedBlocksBuf, UAV);
            cptCtx.TransitionResource(_renderBlockVol, UAV);
            cptCtx.TransitionResource(_fuseBlockVol, UAV);
            cptCtx.TransitionResource(_newFuseBlocksBuf, UAV);
            cptCtx.TransitionResource(_freedFuseBlocksBuf, UAV);
            cptCtx.TransitionResource(_updateBlocksBuf, csSRV);
            cptCtx.TransitionResource(_indirectParams, IARG);
            cptCtx.SetPipelineState(
                _cptBlockVolUpdate_Pass3[buf.type][_TGSize]);
            Bind(cptCtx, 2, 0, 2, buf.UAV);
            Bind(cptCtx, 2, 2, 1, &_fuseBlockVol.GetUAV());
            Bind(cptCtx, 2, 3, 1, &_newFuseBlocksBuf.GetUAV());
            Bind(cptCtx, 2, 4, 1, &_freedFuseBlocksBuf.GetUAV());
            Bind(cptCtx, 2, 5, 1, &_occupiedBlocksBuf.GetUAV());
            Bind(cptCtx, 2, 6, 1, &_renderBlockVol.GetUAV());
            Bind(cptCtx, 3, 0, 1, &pDepthTex->GetSRV());
            if (!_typedLoadSupported) {
                Bind(cptCtx, 3, 1, 2, buf.SRV);
            }
            Bind(cptCtx, 3, 3, 1, &_updateBlocksBuf.GetSRV());
            cptCtx.DispatchIndirect(_indirectParams, 12);
        }
        // Read queue buffer counter back for debugging
        if (_readBackGPUBufStatus) {
            Graphics::g_cmdListMngr.WaitForFence(_readBackFence);
            static D3D12_RANGE range = {};
            _debugBuf.Map(nullptr, reinterpret_cast<void**>(&_readBackPtr));
            memcpy(_readBackData, _readBackPtr, 4 * sizeof(uint32_t));
            _debugBuf.Unmap(nullptr);
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
        {
            GPU_PROFILE(cptCtx, L"OccupiedQPrepare");
            cptCtx.TransitionResource(_indirectParams, UAV);
            cptCtx.SetPipelineState(_cptBlockQUpdate_Prepare);
            Bind(cptCtx, 2, 0, 1, &_newFuseBlocksBuf.GetCounterUAV(cmdCtx));
            Bind(cptCtx, 2, 1, 1, &_freedFuseBlocksBuf.GetCounterUAV(cmdCtx));
            Bind(cptCtx, 2, 2, 1, &_indirectParams.GetUAV());
            Bind(cptCtx, 2, 3, 1, &_jobParamBuf.GetUAV());
            cptCtx.Dispatch1D(1, WARP_SIZE);
        }
        // Filling in NewOccupiedBlocks into FreeSlots in OccupiedBlockQueue
        {
            GPU_PROFILE(cptCtx, L"FreedQFillIn");
            cptCtx.TransitionResource(_occupiedBlocksBuf, UAV);
            cptCtx.TransitionResource(_newFuseBlocksBuf, csSRV);
            cptCtx.TransitionResource(_freedFuseBlocksBuf, csSRV);
            cptCtx.TransitionResource(_jobParamBuf, csSRV);
            cptCtx.TransitionResource(_indirectParams, IARG);
            cptCtx.SetPipelineState(_cptBlockQUpdate_Pass1);
            Bind(cptCtx, 2, 0, 1, &_occupiedBlocksBuf.GetUAV());
            Bind(cptCtx, 3, 0, 1, &_newFuseBlocksBuf.GetSRV());
            Bind(cptCtx, 3, 1, 1, &_freedFuseBlocksBuf.GetSRV());
            Bind(cptCtx, 3, 2, 1, &_jobParamBuf.GetSRV());
            cptCtx.DispatchIndirect(_indirectParams, 24);
        }
        // Appending new occupied blocks to OccupiedBlockQueue
        {
            GPU_PROFILE(cptCtx, L"OccupiedQAppend");
            cptCtx.SetPipelineState(_cptBlockQUpdate_Pass2);
            Bind(cptCtx, 2, 0, 1, &_occupiedBlocksBuf.GetUAV());
            Bind(cptCtx, 3, 0, 1, &_newFuseBlocksBuf.GetSRV());
            Bind(cptCtx, 3, 1, 1, &_jobParamBuf.GetSRV());
            cptCtx.DispatchIndirect(_indirectParams, 36);
        }
        // Resolve OccupiedBlockQueue for next VoxelUpdate DispatchIndirect
        {
            GPU_PROFILE(cptCtx, L"OccupiedQResolve");
            cptCtx.TransitionResource(_indirectParams, UAV);
            cptCtx.SetPipelineState(_cptBlockQUpdate_Resolve);
            Bind(cptCtx, 2, 0, 1, &_indirectParams.GetUAV());
            Bind(cptCtx, 3, 0, 1, &_occupiedBlocksBuf.GetCounterSRV(cmdCtx));
            cptCtx.Dispatch1D(1, WARP_SIZE);
        }
    } else {
        GPU_PROFILE(cmdCtx, L"Volume Updating");
        _CleanRenderBlockVol(cptCtx);
        cptCtx.SetPipelineState(_cptUpdatePSO[buf.type][type]);
        Bind(cptCtx, 2, 0, 2, buf.UAV);
        Bind(cptCtx, 2, 2, 1, &_renderBlockVol.GetUAV());
        Bind(cptCtx, 3, 0, 1, &pDepthTex->GetSRV());
        if (!_typedLoadSupported) {
            Bind(cptCtx, 3, 1, 2, buf.SRV);
        }
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
    gfxCtx.SetRenderTargets(1, &_nearFarTex.GetRTV());
    Bind(gfxCtx, 3, 1, 1, &_renderBlockVol.GetSRV());
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
    Bind(gfxCtx, 3, 0, 1, buf.SRV);
    if (_useStepInfoTex) {
        Bind(gfxCtx, 3, 1, 1, &_nearFarTex.GetSRV());
        Bind(gfxCtx, 3, 2, 1, &_renderBlockVol.GetSRV());
    }
    if (toOutTex) {
        gfxCtx.SetRenderTargets(1, &_extractedDepth.GetRTV());
    } else {
        gfxCtx.SetRenderTargets(1, &Graphics::g_SceneColorBuffer.GetRTV(),
            Graphics::g_SceneDepthBuffer.GetDSV());
    }
    gfxCtx.Draw(3);
}

void
TSDFVolume::_RenderBrickGrid(GraphicsContext& gfxCtx)
{
    gfxCtx.SetPipelineState(_gfxStepInfoDebugPSO);
    gfxCtx.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINESTRIP);
    gfxCtx.SetRenderTargets(1, &Graphics::g_SceneColorBuffer.GetRTV(),
        Graphics::g_SceneDepthBuffer.GetDSV());
    Bind(gfxCtx, 3, 1, 1, &_renderBlockVol.GetSRV());
    gfxCtx.SetIndexBuffer(_cubeLineStripIB.IndexBufferView());
    const uint3 xyz = _volParam->u3VoxelReso;
    const uint ratio = _volParam->uVoxelRenderBlockRatio;
    uint BrickCount = xyz.x * xyz.y * xyz.z / ratio / ratio / ratio;
    gfxCtx.DrawIndexedInstanced(CUBE_LINESTRIP_LENGTH, BrickCount, 0, 0, 0);
}

template<class T>
void
TSDFVolume::_UpdateConstantBuffer(T& ctx)
{
    BindCB(ctx, 0, sizeof(_cbPerFrame), (void*)&_cbPerFrame);
    BindCB(ctx, 1, sizeof(_cbPerCall), (void*)&_cbPerCall);
}