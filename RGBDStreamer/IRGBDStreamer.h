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
	uint64_t CaptureTimeStamp = 0;
	uint16_t Width = 0;
	uint16_t Height = 0;
	uint8_t* pData = nullptr;
	uint32_t Size = 0;

	~FrameData()
	{
		CaptureTimeStamp = 0;
		Size = 0;
		Width = 0;
		Height = 0;
	}
};

class RGBDSTREAMDLL_API IRGBDStreamer
{
public:
	virtual ~IRGBDStreamer() {}
	virtual void GetColorReso( uint16_t& Width, uint16_t& Height) const = 0;
	virtual void GetDepthReso( uint16_t& Width, uint16_t& Height ) const = 0;
	virtual void GetInfraredReso( uint16_t& Width, uint16_t& Height ) const = 0;
	virtual void StartStream( bool EnableColor, bool EnableDepth, bool EnableInfrared ) = 0;
	virtual void StopStream() = 0;
	virtual void GetFrames( FrameData&ColorFrame, FrameData& DepthFrame, FrameData& InfraredFrame ) = 0;
};

class RGBDSTREAMDLL_API StreamFactory {
public:
	static IRGBDStreamer* createFromKinect2();
};