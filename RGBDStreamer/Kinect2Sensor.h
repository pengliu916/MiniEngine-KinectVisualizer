#pragma once

#ifdef RGBDSTREAMDLL_EXPORTS
#define RGBDSTREAMDLL_API __declspec(dllexport) 
#else
#define RGBDSTREAMDLL_API __declspec(dllimport) 
#endif

class RGBDSTREAMDLL_API Kinect2Sensor : public IRGBDStreamer
{
public:
	Kinect2Sensor();
	~Kinect2Sensor();
	virtual void GetColorReso( uint16_t& Width, uint16_t& Height ) const override;
	virtual void GetDepthReso( uint16_t& Width, uint16_t& Height ) const override;
	virtual void GetInfraredReso( uint16_t& Width, uint16_t& Height ) const override;
	virtual void StartStream( bool EnableColor, bool EnableDepth, bool EnableInfrared ) override;
	virtual void StopStream() override;
	virtual void GetFrames( FrameData& ColorFrame, FrameData& DepthFrame, FrameData& InfraredFrame ) override;

protected:
	// Current Kinect
	IKinectSensor*				_pKinect2Sensor;

	// Frame reader
	IMultiSourceFrameReader*	_pMultiSourceFrameReader;

	// Frame data
	FrameData					_pColorFrame[STREAM_BUFFER_COUNT];
	FrameData					_pDepthFrame[STREAM_BUFFER_COUNT];
	FrameData					_pInfraredFrame[STREAM_BUFFER_COUNT];

	// Frame size
	uint16_t					_ColorWidh = 1920;
	uint16_t					_ColorHeight = 1080;
	uint16_t					_DepthWidth = 512;
	uint16_t					_DepthHeight = 424;
	uint16_t					_InfraredWidth = 512;
	uint16_t					_InfraredHeight = 424;

	HRESULT Initialize();
	void Shutdown();

private:
	// Buffer control
	uint8_t						_WritingIdx = 1;
	std::atomic<uint8_t>		_LatestReadableIdx = 0;
	std::atomic<uint8_t>		_ReadingIdx = 0;

	// Background thread control
	std::atomic_bool			_Streaming;
	std::thread					_BackGroundThread;

	// Background thread procedure
	void FrameAcquireLoop( bool EnableColor, bool EnableDepth, bool EnableInfrared );
};