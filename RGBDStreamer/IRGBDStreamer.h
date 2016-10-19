#pragma once

#ifdef RGBDSTREAMDLL_EXPORTS
#define RGBDSTREAMDLL_API __declspec(dllexport) 
#else
#define RGBDSTREAMDLL_API __declspec(dllimport) 
#endif

#ifndef STREAM_BUFFER_COUNT
#define STREAM_BUFFER_COUNT 10
#endif

struct RGBDSTREAMDLL_API FrameData
{
    uint64_t captureTimeStamp = 0;
    uint8_t* pData = nullptr;
    uint32_t Size = 0;

    ~FrameData()
    {
        captureTimeStamp = 0;
        Size = 0;
    }
};

class RGBDSTREAMDLL_API IRGBDStreamer
{
public:
    enum BufferType
    {
        kColor = 0,
        kDepth = 1,
        kInfrared = 2,
        kNumBufferTypes
    };

    virtual ~IRGBDStreamer() {}
    virtual void GetColorReso(uint16_t& Width, uint16_t& Height) const = 0;
    virtual void GetDepthReso(uint16_t& Width, uint16_t& Height) const = 0;
    virtual void GetInfraredReso(uint16_t& Width, uint16_t& Height) const = 0;
    virtual void StartStream() = 0;
    virtual void StopStream() = 0;
    // if new frame is not ready, all buffer contain previous frame and 
    // function will return false, otherwise return true
    virtual bool GetNewFrames(FrameData& ColorFrame, FrameData& DepthFrame,
        FrameData& InfraredFrame) = 0;
};

class RGBDSTREAMDLL_API StreamFactory {
public:
    static IRGBDStreamer* createFromKinect2(
        bool EnableColor, bool EnableDepth, bool EnableInfrared);
};