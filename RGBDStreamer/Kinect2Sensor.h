#pragma once

#ifdef RGBDSTREAMDLL_EXPORTS
#define RGBDSTREAMDLL_API __declspec(dllexport) 
#else
#define RGBDSTREAMDLL_API __declspec(dllimport) 
#endif

class RGBDSTREAMDLL_API Kinect2Sensor : public IRGBDStreamer
{
public:
	Kinect2Sensor( bool EnableColor, bool EnableDepth, bool EnableInfrared );
	~Kinect2Sensor();
	virtual void GetColorReso( uint16_t& Width, uint16_t& Height ) const override;
	virtual void GetDepthReso( uint16_t& Width, uint16_t& Height ) const override;
	virtual void GetInfraredReso( uint16_t& Width, uint16_t& Height ) const override;
	virtual void StartStream() override;
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
	uint16_t					_ColorWidth = 0;
	uint16_t					_ColorHeight = 0;
	uint16_t					_DepthWidth = 0;
	uint16_t					_DepthHeight = 0;
	uint16_t					_InfraredWidth = 0;
	uint16_t					_InfraredHeight = 0;

	// Channel enabled
	bool						_DepthEnabled = false;
	bool						_ColorEnabled = false;
	bool						_InfraredEnabled = false;

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

	// Frame arrival event handle
	WAITABLE_HANDLE				_hFrameArrivalEvent;

	// Background thread procedure
	void FrameAcquireLoop();
	HRESULT ProcessingFrames( IMultiSourceFrameArrivedEventArgs* pArgs );
	HRESULT ProcessDepthFrame( IDepthFrameReference* pDepthFrameRef );
	HRESULT ProcessColorFrame( IColorFrameReference* pColorFrameRef );
	HRESULT ProcessInfraredFrame( IInfraredFrameReference* pInfraredFrameRef );
};