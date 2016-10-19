#pragma once

#ifdef RGBDSTREAMDLL_EXPORTS
#define RGBDSTREAMDLL_API __declspec(dllexport) 
#else
#define RGBDSTREAMDLL_API __declspec(dllimport) 
#endif

class RGBDSTREAMDLL_API Kinect2Sensor : public IRGBDStreamer
{
public:
    Kinect2Sensor(bool enableColor, bool enableDepth, bool enableInfrared);
    ~Kinect2Sensor();
    virtual void GetColorReso(uint16_t& width, uint16_t& height) const override;
    virtual void GetDepthReso(uint16_t& width, uint16_t& height) const override;
    virtual void GetInfraredReso(
        uint16_t& width, uint16_t& height) const override;
    virtual void StartStream() override;
    virtual void StopStream() override;
    virtual bool GetNewFrames(FrameData& colorFrame, FrameData& depthFrame,
        FrameData& infraredFrame) override;

protected:
    // Current Kinect
    IKinectSensor* _pKinect2Sensor;

    // Frame reader
    IMultiSourceFrameReader* _pMultiSourceFrameReader;

    // Frame data
    FrameData _pFrames[kNumBufferTypes][STREAM_BUFFER_COUNT];

    // Channel enabled
    bool _depthEnabled = false;
    bool _colorEnabled = false;
    bool _infraredEnabled = false;

    HRESULT Initialize();
    void Shutdown();

private:
    class thread_guard
    {
    public:
        thread_guard(std::thread& _t) :t(std::move(_t)) {}
        thread_guard(std::thread&& _t) :t(std::move(_t)) {}
        ~thread_guard() { if (t.joinable())t.join(); }

        thread_guard(thread_guard const&) = delete;
        thread_guard& operator=(thread_guard const&) = delete;
    private:
        std::thread t;
    };

    // Buffer control
    uint8_t _writingIdx = 1;
    std::atomic<uint8_t> _latestReadableIdx = 0;
    std::atomic<uint8_t> _readingIdx = 0;

    // Background thread control
    std::atomic_bool _streaming;
    std::unique_ptr<thread_guard> _backGroundThread;

    // Frame arrival event handle
    WAITABLE_HANDLE _hFrameArrivalEvent;

    // Background thread procedure
    void FrameAcquireLoop();
    HRESULT ProcessingFrames(IMultiSourceFrameArrivedEventArgs* pArgs);
    HRESULT ProcessDepthFrame(IDepthFrameReference* pDepthFrameRef);
    HRESULT ProcessColorFrame(IColorFrameReference* pColorFrameRef);
    HRESULT ProcessInfraredFrame(IInfraredFrameReference* pInfraredFrameRef);
};