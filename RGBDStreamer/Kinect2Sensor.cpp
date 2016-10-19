#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#pragma warning(push)
#pragma warning(disable : 4005 4668)
#include <stdint.h>
#pragma warning(pop)


#include <windows.h>
#include <Kinect.h>
#include <thread>
#include <atomic>
#include <chrono>
#include <cassert>
#include <tchar.h>
#include <comdef.h>

#include "IRGBDStreamer.h"
#include "Kinect2Sensor.h"

#define __FILENAME__ (wcsrchr (_T(__FILE__), L'\\') \
? wcsrchr (_T(__FILE__), L'\\') + 1 : _T(__FILE__))
#if defined(DEBUG) || defined(_DEBUG)
#ifndef V
#define V(x) { hr = (x); if( FAILED(hr) ) \
{ Trace( __FILENAME__, (DWORD)__LINE__, hr, L###x); __debugbreak(); } }
#endif //#ifndef V
#ifndef VRET
#define VRET(x) { hr = (x); if( FAILED(hr) ) \
{ return Trace( __FILENAME__, (DWORD)__LINE__, hr, L###x); __debugbreak(); } }
#endif //#ifndef VRET
#ifndef W
#define W(x) { hr = (x); if( FAILED(hr) ) \
{ Trace( __FILENAME__, (DWORD)__LINE__, hr, L###x); } }
#endif //#ifndef W
#ifndef WRET
#define WRET(x) { hr = (x); if( FAILED(hr) ) \
{ return Trace( __FILENAME__, (DWORD)__LINE__, hr, L###x); } }
#endif //#ifndef WRET
#else
#ifndef V
#define V(x) { hr = (x); }
#endif //#ifndef V
#ifndef VRET
#define VRET(x) { hr = (x); if( FAILED(hr) ) { return hr; } }
#endif //#ifndef VRET
#ifndef W
#define W(x) { hr = (x); }
#endif //#ifndef W
#ifndef WRET
#define WRET(x) { hr = (x); if( FAILED(hr) ) { return hr; } }
#endif //#ifndef WRET
#endif //#if defined(DEBUG) || defined(_DEBUG)

namespace {
    // Frame size
    const uint16_t _colorWidth = 1920;
    const uint16_t _colorHeight = 1080;
    const uint16_t _depthWidth = 512;
    const uint16_t _depthHeight = 424;
    const uint16_t _infraredWidth = 512;
    const uint16_t _infraredHeight = 424;

    const uint32_t _colorBufSize =
        _colorWidth * _colorHeight * 4 * sizeof(uint8_t);
    const uint32_t _depthBufSize =
        _depthWidth * _depthHeight * sizeof(uint16_t);
    const uint32_t _infraredBufSize =
        _infraredWidth * _infraredHeight * sizeof(uint16_t);

    inline HRESULT Trace(const wchar_t* strFile,
        DWORD dwLine, HRESULT hr, const wchar_t* strMsg) {
        wchar_t szBuffer[512];
        int offset = 0;
        if (strFile) {
            offset += wsprintf(szBuffer,
                L"line %u in file %s\n", dwLine, strFile);
        }
        offset +=
            wsprintf(szBuffer + offset, L"Calling: %s failed!\n ", strMsg);
        _com_error err(hr);
        wsprintf(szBuffer + offset, err.ErrorMessage());
        OutputDebugString(szBuffer);
        return hr;
    }

    void SetThreadName(const char* name) {
        // http://msdn.microsoft.com/en-us/library/xcb2z8hs(v=vs.110).aspx
    #pragma pack(push,8)
        typedef struct tagTHREADNAME_INFO {
            DWORD dwType; // must be 0x1000
            LPCSTR szName; // pointer to name (in user addr space)
            DWORD dwThreadID; // thread ID (-1=caller thread)
            DWORD dwFlags; // reserved for future use, must be zero
        } THREADNAME_INFO;
    #pragma pack(pop)

        THREADNAME_INFO info;
        info.dwType = 0x1000;
        info.szName = name;
        info.dwThreadID = (DWORD)-1;
        info.dwFlags = 0;
        __try {
            RaiseException(0x406D1388, 0,
                sizeof(info) / sizeof(ULONG_PTR), (ULONG_PTR*)&info);
        } __except (EXCEPTION_CONTINUE_EXECUTION) {}
    }
}
HRESULT
Kinect2Sensor::Initialize()
{
    HRESULT hr;
    WRET(GetDefaultKinectSensor(&_pKinect2Sensor));

    int SourceType = FrameSourceTypes_None;

    if (_colorEnabled) SourceType |= FrameSourceTypes_Color;
    if (_depthEnabled) SourceType |= FrameSourceTypes_Depth;
    if (_infraredEnabled) SourceType |= FrameSourceTypes_Infrared;

    WRET(_pKinect2Sensor->Open());
    WRET(_pKinect2Sensor->OpenMultiSourceFrameReader(
        SourceType, &_pMultiSourceFrameReader));
    WRET(_pMultiSourceFrameReader->SubscribeMultiSourceFrameArrived(
        &_hFrameArrivalEvent));
    return hr;
}

void
Kinect2Sensor::Shutdown()
{
    if (_pMultiSourceFrameReader) {
        _pMultiSourceFrameReader->UnsubscribeMultiSourceFrameArrived(
            _hFrameArrivalEvent);
        _pMultiSourceFrameReader->Release();
        _pMultiSourceFrameReader = nullptr;
    }
    BOOLEAN opened = false;
    if ( _pKinect2Sensor && 
        SUCCEEDED(_pKinect2Sensor->get_IsOpen(&opened)) && opened) {
        _pKinect2Sensor->Close();
        _pKinect2Sensor->Release();
        _pKinect2Sensor = nullptr;
    }
}

Kinect2Sensor::Kinect2Sensor(
    bool enableColor, bool enableDepth, bool enableInfrared)
{
    HRESULT hr = S_OK;
    _hFrameArrivalEvent = NULL;
    _depthEnabled = enableDepth;
    _colorEnabled = enableColor;
    _infraredEnabled = enableInfrared;
    _streaming.store(false, std::memory_order_relaxed);
    V(Initialize());
    return;
}

Kinect2Sensor::~Kinect2Sensor()
{
    _streaming.store(false, std::memory_order_relaxed);
    Shutdown();
    for (uint8_t i = 0; i < kNumBufferTypes; ++i) {
        for (uint8_t j = 0; j < 2; ++j) {
            if (_pFrames[i][j].pData) {
                delete _pFrames[i][j].pData;
                _pFrames[i][j].pData = nullptr;
            }
        }
    }
}

void
Kinect2Sensor::GetColorReso(uint16_t& width, uint16_t& height) const
{
    width = _colorWidth; height = _colorHeight;
}

void
Kinect2Sensor::GetDepthReso(uint16_t& width, uint16_t& height) const
{
    width = _depthWidth; height = _depthHeight;
}

void
Kinect2Sensor::GetInfraredReso(uint16_t& width, uint16_t& height) const
{
    width = _infraredWidth; height = _infraredHeight;
}

void
Kinect2Sensor::StartStream()
{
    if (!_streaming.load(std::memory_order_relaxed)) {
        _streaming.store(true, std::memory_order_release);
        _backGroundThread = std::unique_ptr<thread_guard>(
            new thread_guard(std::thread(
                &Kinect2Sensor::FrameAcquireLoop, this)));
    }
}

void
Kinect2Sensor::StopStream()
{
    if (_streaming.load(std::memory_order_relaxed)) {
        _streaming.store(false, std::memory_order_relaxed);
    }
}

bool
Kinect2Sensor::GetNewFrames(
    FrameData& ColorFrame, FrameData& DepthFrame, FrameData& InfraredFrame)
{
    uint8_t preReadingIdx = _readingIdx.load(std::memory_order_acquire);
    uint8_t newReadingIdx = _latestReadableIdx.load(std::memory_order_acquire);
    _readingIdx.store(newReadingIdx, std::memory_order_release);
    ColorFrame = _pFrames[kColor][newReadingIdx];
    DepthFrame = _pFrames[kDepth][newReadingIdx];
    InfraredFrame = _pFrames[kInfrared][newReadingIdx];
    return preReadingIdx != newReadingIdx;
}

void
Kinect2Sensor::FrameAcquireLoop()
{
    SetThreadName("KinectBackground Thread");

    INT64 ColTimeStampPreFrame = 0;
    INT64 DepTimeStampPreFrame = 0;
    INT64 InfTimeStampPreFrame = 0;

    HRESULT hr = S_OK;
    while (_streaming.load(std::memory_order_consume)) {
        while (FAILED(hr)) {
            Shutdown();
            std::this_thread::sleep_for(std::chrono::seconds(2));
            hr = Initialize();
        }
        int idx = WaitForSingleObject(
            reinterpret_cast<HANDLE>(_hFrameArrivalEvent), 3000);
        switch (idx) {
        case WAIT_TIMEOUT:
            hr = E_FAIL;
            OutputDebugString(L"Wait Kinect Frame Timeout\n");
            continue;
        case WAIT_OBJECT_0:
            IMultiSourceFrameArrivedEventArgs *pFrameArgs = nullptr;
            V(_pMultiSourceFrameReader->GetMultiSourceFrameArrivedEventData(
                _hFrameArrivalEvent, &pFrameArgs));
            ProcessingFrames(pFrameArgs);
            pFrameArgs->Release();
        }
    }
}

HRESULT
Kinect2Sensor::ProcessingFrames(IMultiSourceFrameArrivedEventArgs* pArgs)
{
    HRESULT hr = S_OK;
    IMultiSourceFrameReference *pFrameReference = nullptr;

    VRET(pArgs->get_FrameReference(&pFrameReference));

    IMultiSourceFrame *pFrame = nullptr;
    VRET(pFrameReference->AcquireFrame(&pFrame));
    if (SUCCEEDED(hr) && _colorEnabled) {
        IColorFrameReference* pColorFrameReference = nullptr;
        VRET(pFrame->get_ColorFrameReference(&pColorFrameReference));
        W(ProcessColorFrame(pColorFrameReference));
        pColorFrameReference->Release();
    }
    if (SUCCEEDED(hr) && _depthEnabled) {
        IDepthFrameReference* pDepthFrameReference = nullptr;
        VRET(pFrame->get_DepthFrameReference(&pDepthFrameReference));
        W(ProcessDepthFrame(pDepthFrameReference));
        pDepthFrameReference->Release();
    }
    if (SUCCEEDED(hr) && _infraredEnabled) {
        IInfraredFrameReference* pInfraredFrameReference = nullptr;
        VRET(pFrame->get_InfraredFrameReference(&pInfraredFrameReference));
        W(ProcessInfraredFrame(pInfraredFrameReference));
        pInfraredFrameReference->Release();
    }
    pFrameReference->Release();

    if (SUCCEEDED(hr)) {
        _latestReadableIdx.store(_writingIdx, std::memory_order_release);
        _writingIdx = (_writingIdx + 1) % STREAM_BUFFER_COUNT;
        while (_readingIdx.load(std::memory_order_acquire) == _writingIdx) {
            std::this_thread::yield();
            if (_streaming.load(std::memory_order_consume) == false) {
                return hr;
            }
        }
    }
    return hr;
}

HRESULT
Kinect2Sensor::ProcessDepthFrame(IDepthFrameReference* pDepthFrameRef)
{
    INT64 DepTimeStamp = 0;
    IDepthFrame* pDepthFrame = NULL;
    HRESULT hr = S_OK;
    WRET(pDepthFrameRef->AcquireFrame(&pDepthFrame));
    V(pDepthFrameRef->get_RelativeTime(&DepTimeStamp));
    // get depth frame data
    FrameData& CurFrame = _pFrames[kDepth][_writingIdx];
    CurFrame.Size = _depthBufSize;
    CurFrame.captureTimeStamp = DepTimeStamp;
    if (!CurFrame.pData) {
        CurFrame.pData = (uint8_t*)std::malloc(_depthBufSize);
    }
    V(pDepthFrame->CopyFrameDataToArray((UINT)(_depthBufSize / 2),
        reinterpret_cast<UINT16*>(CurFrame.pData)));
    pDepthFrame->Release();
    return hr;
}

HRESULT
Kinect2Sensor::ProcessColorFrame(IColorFrameReference* pColorFrameRef)
{
    INT64 ColTimeStamp = 0;
    IColorFrame* pColorFrame = NULL;
    HRESULT hr = S_OK;
    WRET(pColorFrameRef->AcquireFrame(&pColorFrame));
    V(pColorFrameRef->get_RelativeTime(&ColTimeStamp));
    // get color frame data
    FrameData& CurFrame = _pFrames[kColor][_writingIdx];
    CurFrame.Size = _colorBufSize;
    CurFrame.captureTimeStamp = ColTimeStamp;
    if (!CurFrame.pData) {
        CurFrame.pData = (uint8_t*)std::malloc(_colorBufSize);
    }
    V(pColorFrame->CopyConvertedFrameDataToArray((UINT)_colorBufSize,
        reinterpret_cast<BYTE*>(CurFrame.pData), ColorImageFormat_Rgba));
    pColorFrame->Release();
    return hr;
}

HRESULT
Kinect2Sensor::ProcessInfraredFrame(IInfraredFrameReference* pInfraredFrameRef)
{
    INT64 InfTimeStamp = 0;
    IInfraredFrame* pInfraredFrame = NULL;
    HRESULT hr = S_OK;
    VRET(pInfraredFrameRef->AcquireFrame(&pInfraredFrame));
    V(pInfraredFrameRef->get_RelativeTime(&InfTimeStamp));
    // get Infrared frame data
    FrameData& CurFrame = _pFrames[kInfrared][_writingIdx];
    CurFrame.Size = _infraredBufSize;
    CurFrame.captureTimeStamp = InfTimeStamp;
    if (!CurFrame.pData) {
        CurFrame.pData = (uint8_t*)std::malloc(_infraredBufSize);
    }
    V(pInfraredFrame->CopyFrameDataToArray((UINT)(_infraredBufSize / 2),
        reinterpret_cast<UINT16*>(CurFrame.pData)));
    pInfraredFrame->Release();
    return hr;
}

IRGBDStreamer*
StreamFactory::createFromKinect2(
    bool enableColor, bool enableDepth, bool enableInfrared)
{
    return new Kinect2Sensor(enableColor, enableDepth, enableInfrared);
}