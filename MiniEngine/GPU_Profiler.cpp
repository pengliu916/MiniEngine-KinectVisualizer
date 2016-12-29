#include "LibraryHeader.h"
#include "DX12Framework.h"
#include "Utility.h"
#include "DXHelper.h"
#include "CommandContext.h"
#include "CmdListMngr.h"
#include "TextRenderer.h"
#include "Graphics.h"
#include "imgui.h"

#include <unordered_map>
#include <string>
#include <vector>
#include <functional>
#include "GPU_Profiler.h"

using namespace Microsoft::WRL;
using namespace std;
using namespace DirectX;

namespace {
enum ShowMode {
    kTotalOnly = 0,
    kMajor = 1,
    kAll = 2,
    kNone = 3,
};

ShowMode _showMode = kMajor;
float _tThreshold = 0.5f;

double _gpuTickTime;
uint8_t _numTimer = 1;
unordered_map<wstring, uint8_t> _mapStrID;
wstring* _names;
XMFLOAT4* _colors;
#if RECORD_TIME_HISTORY
float** _history;
#endif
float* _avgs;
uint64_t* _cpuStampBuf;

ID3D12Resource* _readBackBuf;
ID3D12QueryHeap* _queryHeap;
uint64_t* _gpuStampBuf;

uint64_t _fenceVal = 0;

RootSignature _rootsig;
GraphicsPSO _gfxPSO;

CRITICAL_SECTION _criticalSection;

vector<uint8_t> _activeTimers;

bool compStartTime(uint8_t i, uint8_t j) {
    return _cpuStampBuf[i * 2] < _cpuStampBuf[j * 2];
}

struct RectAttr {
    XMFLOAT4 TLBR;
    XMFLOAT4 Col;
};

RectAttr* _RectData;
uint16_t _BGMargin;
uint16_t _entryMargin;
uint16_t _entryHeight;
uint16_t _wordHeight;
uint16_t _maxWidth;
uint16_t _wordSpace;
}

void
GPU_Profiler::Initialize()
{
    _BGMargin = 5;
    _entryMargin = 5;
    _wordHeight = 15;
    _entryHeight = 2 * _entryMargin + _wordHeight;
    _maxWidth = 500;
    _wordSpace = 250;

    // Initialize the array to store all timer name
    _names = new wstring[MAX_TIMER_COUNT];

    // Initialize the array to copy from timer buffer
    _cpuStampBuf = new uint64_t[MAX_TIMER_COUNT];

    _colors = new XMFLOAT4[MAX_TIMER_COUNT];

    _RectData = new RectAttr[MAX_TIMER_COUNT * 2];
#if RECORD_TIME_HISTORY
    _history = new float*[MAX_TIMER_COUNT];
    for (int i = 0; i < MAX_TIMER_COUNT; ++i) {
        _history[i] = new float[MAX_TIMER_HISTORY];
        memset((void*)_history[i], 0, sizeof(float) * MAX_TIMER_HISTORY);
    }
#endif
    _avgs = new float[MAX_TIMER_COUNT];
    memset((void*)_avgs, 0, sizeof(float) * MAX_TIMER_COUNT);

    _activeTimers.reserve(MAX_TIMER_COUNT);

    // Initialize output critical section
    InitializeCriticalSection(&_criticalSection);
}

HRESULT
GPU_Profiler::CreateResource()
{
    HRESULT hr;
    uint64_t freq;
    Graphics::g_cmdListMngr.GetCommandQueue()->GetTimestampFrequency(&freq);
    _gpuTickTime = 1000.0 / static_cast<double>(freq);

    D3D12_HEAP_PROPERTIES HeapProps;
    HeapProps.Type = D3D12_HEAP_TYPE_READBACK;
    HeapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    HeapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    HeapProps.CreationNodeMask = 1;
    HeapProps.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC BufferDesc;
    BufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    BufferDesc.Alignment = 0;
    BufferDesc.Width = sizeof(uint64_t) * MAX_TIMER_COUNT * 2;
    BufferDesc.Height = 1;
    BufferDesc.DepthOrArraySize = 1;
    BufferDesc.MipLevels = 1;
    BufferDesc.Format = DXGI_FORMAT_UNKNOWN;
    BufferDesc.SampleDesc.Count = 1;
    BufferDesc.SampleDesc.Quality = 0;
    BufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    BufferDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    VRET(Graphics::g_device->CreateCommittedResource(
        &HeapProps, D3D12_HEAP_FLAG_NONE, &BufferDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&_readBackBuf)));
    _readBackBuf->SetName(L"GPU_Profiler Readback Buffer");

    D3D12_QUERY_HEAP_DESC QueryHeapDesc;
    QueryHeapDesc.Count = MAX_TIMER_COUNT * 2;
    QueryHeapDesc.NodeMask = 1;
    QueryHeapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    VRET(Graphics::g_device->CreateQueryHeap(
        &QueryHeapDesc, IID_PPV_ARGS(&_queryHeap)));
    PRINTINFO("QueryHeap created");
    _queryHeap->SetName(L"GPU_Profiler QueryHeap");

    // Create resource for drawing perf graph
    _rootsig.Reset(1);
    _rootsig[0].InitAsBufferSRV(0, D3D12_SHADER_VISIBILITY_VERTEX);
    _rootsig.Finalize(L"GPU_Profiler RootSignature");

    _gfxPSO.SetRootSignature(_rootsig);
    _gfxPSO.SetRasterizerState(Graphics::g_RasterizerDefault);
    _gfxPSO.SetBlendState(Graphics::g_BlendTraditional);
    _gfxPSO.SetDepthStencilState(Graphics::g_DepthStateDisabled);
    _gfxPSO.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    _gfxPSO.SetRenderTargetFormats(
        1, &Graphics::g_pDisplayPlanes[0].GetFormat(), DXGI_FORMAT_UNKNOWN);

    ComPtr<ID3DBlob> vertexShader;
    ComPtr<ID3DBlob> pixelShader;

    uint32_t compileFlags = 0;

    VRET(Graphics::CompileShaderFromFile(Core::GetAssetFullPath(
        _T("GPU_Profiler.hlsl")).c_str(), nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE, "vsmain", "vs_5_0",
        compileFlags, 0, &vertexShader));
    VRET(Graphics::CompileShaderFromFile(Core::GetAssetFullPath(
        _T("GPU_Profiler.hlsl")).c_str(), nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE, "psmain", "ps_5_0",
        compileFlags, 0, &pixelShader));

    _gfxPSO.SetVertexShader(
        vertexShader->GetBufferPointer(), vertexShader->GetBufferSize());
    _gfxPSO.SetPixelShader(
        pixelShader->GetBufferPointer(), pixelShader->GetBufferSize());
    _gfxPSO.Finalize();

    return S_OK;
}

void
GPU_Profiler::ShutDown()
{
    if (_readBackBuf != nullptr) {
        _readBackBuf->Release();
        _readBackBuf = nullptr;
    }
    if (_queryHeap != nullptr) {
        _queryHeap->Release();
        _queryHeap = nullptr;
    }
    _activeTimers.clear();
    delete[] _RectData;
    _RectData = nullptr;
    delete[] _names;
    _names = nullptr;
    delete[] _colors;
    _colors = nullptr;
    delete[] _cpuStampBuf;
    _cpuStampBuf = nullptr;
#if RECORD_TIME_HISTORY
    for (int i = 0; i < MAX_TIMER_COUNT; ++i) {
        delete[] _history[i];
    }
    delete[] _history;
#endif
    delete[] _avgs;

    DeleteCriticalSection(&_criticalSection);
}

void
GPU_Profiler::ProcessAndReadback(CommandContext& EngineContext)
{
    Graphics::g_cmdListMngr.WaitForFence(_fenceVal);

    HRESULT hr;
    D3D12_RANGE range;
    range.Begin = 0;
    range.End = MAX_TIMER_COUNT * 2 * sizeof(uint64_t);
    V(_readBackBuf->Map(0, &range, reinterpret_cast<void**>(&_gpuStampBuf)));
    memcpy(_cpuStampBuf, _gpuStampBuf, _numTimer * 2 * sizeof(uint64_t));
    D3D12_RANGE EmptyRange = {};
    _readBackBuf->Unmap(0, &EmptyRange);

    // based on previous frame end timestamp, creat an active timer idx vector
    _activeTimers.clear();
    uint64_t preEndTime = _cpuStampBuf[0];
#if RECORD_TIME_HISTORY
    _history[0][Core::g_frameCount % MAX_TIMER_HISTORY] = (float)ReadTimer(0);
    for (uint8_t idx = 1; idx < _numTimer; ++idx) {
        if (_cpuStampBuf[idx * 2] >= preEndTime) {
            _activeTimers.push_back(idx);
            _history[idx][Core::g_frameCount % MAX_TIMER_HISTORY] =
                (float)ReadTimer(idx);
        } else {
            _history[idx][Core::g_frameCount % MAX_TIMER_HISTORY] = 0.f;
        }
    }
    for (uint8_t idx = 0; idx < _numTimer; ++idx) {
        uint8_t count = 0;
        float avg = 0.f;
        for (uint8_t i = 0; i < MAX_TIMER_HISTORY; ++i) {
            float val = _history[idx][i];
            if (val > 0.f) {
                ++count;
                avg += val;
            }
        }
        if (count > 0) {
            _avgs[idx] = avg / count;
        }
    }
#else
    _avgs[0] = (_avgs[0] * AVG_COUNT_FACTOR + (float)ReadTimer(0)) /
        (AVG_COUNT_FACTOR + 1);
    for (uint8_t idx = 1; idx < _numTimer; ++idx) {
        if (_cpuStampBuf[idx * 2] >= preEndTime) {
            _activeTimers.push_back(idx);
            _avgs[idx] =
                (_avgs[idx] * AVG_COUNT_FACTOR + (float)ReadTimer(idx)) /
                (AVG_COUNT_FACTOR + 1);
        } else {
            _avgs[idx] = 0.f;
        }
    }
#endif
    // sort timer based on timer's start time
    sort(_activeTimers.begin(), _activeTimers.end(), compStartTime);

    EngineContext.InsertTimeStamp(_queryHeap, 1);
    EngineContext.ResolveTimeStamps(_readBackBuf, _queryHeap, 2 * _numTimer);
    EngineContext.InsertTimeStamp(_queryHeap, 0);
}

uint16_t
GPU_Profiler::FillVertexData()
{
    float fMarginPixel = 10.f;
    float fViewWidth = (float)Core::g_config.swapChainDesc.Width;
    float fViewHeight = (float)Core::g_config.swapChainDesc.Height;
    const float vpX = 0.0f;
    const float vpY = 0.0f;
    const float scaleX = 2.0f / fViewWidth;
    const float scaleY = -2.0f / fViewHeight;
    const float offsetX = -vpX*scaleX - 1.f;
    const float offsetY = -vpY*scaleY + 1.f;

    auto Corner = [&](UINT TLx, UINT TLy, UINT BRx, UINT BRy)->XMFLOAT4 {
        return XMFLOAT4(TLx * scaleX + offsetX, TLy * scaleY + offsetY,
            BRx * scaleX + offsetX, BRy * scaleY + offsetY);
    };

    uint8_t uActiveCnt = (uint8_t)_activeTimers.size();
    uint8_t uRectID = 2;

    float fScale = _maxWidth / 33.f;
    uint8_t showCnt = 0;
    if (uActiveCnt > 0) {
        uint16_t CurStartX = _BGMargin + _entryMargin + _wordSpace;
        uint16_t CurStartY = _BGMargin + _entryMargin;
        CurStartY += _entryHeight;
        double startT, endT, durT;
        auto AddRect = [&](uint16_t idx, bool bToTotalBar) {
            uint16_t y = bToTotalBar ? _BGMargin + _entryMargin : CurStartY;
            _RectData[uRectID].TLBR = Corner(
                CurStartX + (UINT)(startT * fScale), y,
                CurStartX + (UINT)(endT * fScale), y + _wordHeight);
            _RectData[uRectID].Col = _colors[_activeTimers[idx]];
            if (!bToTotalBar) CurStartY += _entryHeight;
            uRectID++;
        };
        for (uint16_t idx = 0; idx < uActiveCnt; idx++) {
            durT = ReadTimer(_activeTimers[idx], &startT, &endT);
            AddRect(idx, true);
            if (_showMode != kTotalOnly &&
                (_showMode != kMajor || durT > _tThreshold)) {
                AddRect(idx, false);
                showCnt++;
            }
        }
    }
    double t = ReadTimer(0);
    _RectData[1].TLBR = Corner(_BGMargin + _entryMargin + _wordSpace,
        _BGMargin + _entryMargin,
        _BGMargin + _entryMargin + _wordSpace + (UINT)(t * fScale),
        _BGMargin + _entryMargin + _wordHeight);
    _RectData[1].Col = XMFLOAT4(0.8f, 0.8f, 1.f, 0.5f);
    showCnt++;
    _RectData[0].TLBR = Corner(_BGMargin, _BGMargin, _maxWidth + _wordSpace,
        _BGMargin + (showCnt)* _entryHeight);
    _RectData[0].Col = XMFLOAT4(0.f, 0.f, 0.f, 0.3f);
    return uRectID;
}

void
GPU_Profiler::DrawStats(GraphicsContext& gfxContext)
{
    if (_showMode == kNone) {
        return;
    }
    GPU_PROFILE(gfxContext, L"DrawPerfGraph");
    uint16_t instanceCount = FillVertexData();
    gfxContext.SetRootSignature(_rootsig);
    gfxContext.SetPipelineState(_gfxPSO);
    gfxContext.SetDynamicSRV(0, sizeof(RectAttr) * _numTimer * 2, _RectData);
    gfxContext.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    gfxContext.SetRenderTargets(
        1, &Graphics::g_pDisplayPlanes[Graphics::g_CurrentDPIdx].GetRTV());
    gfxContext.SetViewport(Graphics::g_DisplayPlaneViewPort);
    gfxContext.SetScisor(Graphics::g_DisplayPlaneScissorRect);
    gfxContext.DrawInstanced(4, instanceCount, 0, 1);

    TextContext txtContext(gfxContext);
    txtContext.Begin();
    float curX = (float)(_BGMargin + _entryMargin);
    float curY = (float)(_BGMargin + _entryMargin);
    txtContext.ResetCursor(curX, curY);
    txtContext.SetTextSize((float)_wordHeight);

    wchar_t temp[128];
    double result = _cpuStampBuf[1] * _gpuTickTime -
        _cpuStampBuf[0] * _gpuTickTime;
    swprintf(temp, L"TotalFrameTime:%4.2fms %4.2fms \0",
        _avgs[0], ReadTimer(0));
    txtContext.DrawString(wstring(temp));
    if (_showMode == kTotalOnly) {
        return;
    }
    curY += _entryHeight;
    txtContext.ResetCursor(curX, curY);
    for (uint32_t idx = 0; idx < _activeTimers.size(); idx++) {
        if (_showMode != kMajor ||
            ReadTimer(_activeTimers[idx]) > _tThreshold) {
            GPU_Profiler::GetTimingStr(_activeTimers[idx], temp);
            txtContext.DrawString(wstring(temp));
            curY += _entryHeight;
            txtContext.ResetCursor(curX, curY);
        }
    }
    txtContext.End();
}

void
GPU_Profiler::RenderGui()
{
    using namespace ImGui;
    if (!CollapsingHeader("GPU Profiler", (const char*)0)) {
        return;
    }
    RadioButton("TotalOnly##Prof", (int*)&_showMode, kTotalOnly); SameLine();
    RadioButton("Major##Prof", (int*)&_showMode, kMajor); SameLine();
    RadioButton("All##Perf", (int*)&_showMode, kAll); SameLine();
    RadioButton("None##Perf", (int*)&_showMode, kNone);
    SliderFloat("Threshold##Perf", &_tThreshold, 0.01f, 2.f, "%.3fms");
}

double
GPU_Profiler::ReadTimer(uint8_t idx, double* start, double* stop)
{
    ASSERT(idx < MAX_TIMER_COUNT);
    uint64_t _frameStart = _cpuStampBuf[0];
    double _start = (_cpuStampBuf[idx * 2] - _frameStart) * _gpuTickTime;
    double _stop = (_cpuStampBuf[idx * 2 + 1] - _frameStart) * _gpuTickTime;
    if (start) *start = _start;
    if (stop) *stop = _stop;
    double _duration = _stop - _start;
    return (_duration > 100.f || _duration < 0.f) ? 0.0 : _stop - _start;
}

void
GPU_Profiler::SetFenceValue(uint64_t fenceValue)
{
    _fenceVal = fenceValue;
}

uint16_t
GPU_Profiler::GetTimingStr(uint8_t idx, wchar_t* outStr)
{
    ASSERT(idx < MAX_TIMER_COUNT);
    if (_names[idx].length() == 0) {
        return 0;
    }
    double result = _cpuStampBuf[idx * 2 + 1] * _gpuTickTime -
        _cpuStampBuf[idx * 2] * _gpuTickTime;
    swprintf(outStr, L"%-15.15s:%4.2fms %4.2fms \0",
        _names[idx].c_str(), _avgs[idx], ReadTimer(idx));
    return (uint16_t)wcslen(outStr);
}

GPUProfileScope::GPUProfileScope(CommandContext& Context, const wchar_t* szName)
    :m_Context(Context)
{
    Context.PIXBeginEvent(szName);
    auto iter = _mapStrID.find(szName);
    if (iter == _mapStrID.end()) {
        CriticalSectionScope lock(&_criticalSection);
        m_idx = _numTimer;
        _names[m_idx] = szName;
        _colors[m_idx] = XMFLOAT4(
            (float)rand() / RAND_MAX, (float)rand() / RAND_MAX,
            (float)rand() / RAND_MAX, 0.8f);
        _mapStrID[szName] = _numTimer++;
    } else {
        m_idx = iter->second;
    }
    ASSERT(m_idx < GPU_Profiler::MAX_TIMER_COUNT);
    m_Context.InsertTimeStamp(_queryHeap, m_idx * 2);
}

GPUProfileScope::~GPUProfileScope()
{
    ASSERT(m_idx < GPU_Profiler::MAX_TIMER_COUNT);
    m_Context.InsertTimeStamp(_queryHeap, m_idx * 2 + 1);
    m_Context.PIXEndEvent();
}