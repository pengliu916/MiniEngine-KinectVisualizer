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

#include "IRGBDStreamer.h"
#include "Kinect2Sensor.h"

#pragma comment (lib, "kinect20.lib")

using namespace std;

// Safe release for interfaces
template<class Interface>
inline void SafeRelease( Interface *& pInterfaceToRelease )
{
	if (pInterfaceToRelease != NULL)
	{
		pInterfaceToRelease->Release();
		pInterfaceToRelease = NULL;
	}
}

void SetThreadName( const char* Name )
{
	// http://msdn.microsoft.com/en-us/library/xcb2z8hs(v=vs.110).aspx
#pragma pack(push,8)
	typedef struct tagTHREADNAME_INFO
	{
		DWORD dwType; // must be 0x1000
		LPCSTR szName; // pointer to name (in user addr space)
		DWORD dwThreadID; // thread ID (-1=caller thread)
		DWORD dwFlags; // reserved for future use, must be zero
	} THREADNAME_INFO;
#pragma pack(pop)

	THREADNAME_INFO info;
	{
		info.dwType = 0x1000;
		info.szName = Name;
		info.dwThreadID = (DWORD)-1;
		info.dwFlags = 0;
	}
	__try
	{
		RaiseException( 0x406D1388, 0, sizeof( info ) / sizeof( ULONG_PTR ), (ULONG_PTR*)&info );
	}
	__except (EXCEPTION_CONTINUE_EXECUTION)
	{
	}
}

HRESULT Kinect2Sensor::Initialize()
{
	HRESULT hr = GetDefaultKinectSensor( &_pKinect2Sensor );
	if (FAILED( hr )) return hr;

	if (SUCCEEDED( hr ))hr = _pKinect2Sensor->Open();
	if (SUCCEEDED( hr ))hr = _pKinect2Sensor->OpenMultiSourceFrameReader(
		FrameSourceTypes::FrameSourceTypes_Depth | FrameSourceTypes::FrameSourceTypes_Color | FrameSourceTypes::FrameSourceTypes_Infrared,
		&_pMultiSourceFrameReader );
	return hr;
}

void Kinect2Sensor::Shutdown()
{
	SafeRelease( _pMultiSourceFrameReader );

	if (_pKinect2Sensor)
	{
		_pKinect2Sensor->Close();
		_pKinect2Sensor->Release();
		_pKinect2Sensor = NULL;
	}
}

Kinect2Sensor::Kinect2Sensor()
{
	_Streaming.store( false, memory_order_relaxed );
	Initialize();
	return;
}

Kinect2Sensor::~Kinect2Sensor()
{
	_Streaming.store( false, memory_order_relaxed );
	if (_BackGroundThread.joinable())
		_BackGroundThread.join();

	Shutdown();
}

void Kinect2Sensor::GetColorReso( uint16_t& Width, uint16_t& Height ) const
{
	Width = _ColorWidh; Height = _ColorHeight;
}

void Kinect2Sensor::GetDepthReso( uint16_t& Width, uint16_t& Height ) const
{
	Width = _DepthWidth; Height = _DepthHeight;
}

void Kinect2Sensor::GetInfraredReso( uint16_t& Width, uint16_t& Height ) const
{
	Width = _InfraredWidth; Height = _InfraredHeight;
}

void Kinect2Sensor::StartStream( bool EnableColor, bool EnableDepth, bool EnableInfrared )
{
	if (!_Streaming.load( memory_order_relaxed ))
	{
		_Streaming.store( true, memory_order_release );
		thread t( &Kinect2Sensor::FrameAcquireLoop, this, EnableColor, EnableDepth, EnableInfrared );
		_BackGroundThread = move( t );
	}
}

void Kinect2Sensor::StopStream()
{
	if (_Streaming.load( memory_order_relaxed ))
	{
		_Streaming.store( false, memory_order_relaxed );
	}
}

void Kinect2Sensor::GetFrames( FrameData& ColorFrame, FrameData& DepthFrame, FrameData& InfraredFrame )
{
	_ReadingIdx.store( _LatestReadableIdx.load( memory_order_acquire ), memory_order_release );
	ColorFrame = _pColorFrame[_ReadingIdx];
	DepthFrame = _pDepthFrame[_ReadingIdx];
	InfraredFrame = _pInfraredFrame[_ReadingIdx];
}

void Kinect2Sensor::FrameAcquireLoop( bool EnableColor, bool EnableDepth, bool EnableInfrared )
{
	SetThreadName( "KinectBackground Thread" );

	HRESULT hr = S_OK;
	while (_Streaming.load( memory_order_consume ))
	{
		while (FAILED( hr ))
		{
			Shutdown();
			this_thread::sleep_for( 2s );
			hr = Initialize();
		}

		IMultiSourceFrame* pMultiSourceFrame = NULL;
		IDepthFrame* pDepthFrame = NULL;
		IColorFrame* pColorFrame = NULL;
		IInfraredFrame* pInfraredFrame = NULL;

		INT64 ColTimeStamp = 0;
		INT64 DepTimeStamp = 0;
		INT64 InfTimeStamp = 0;

		HRESULT hr = _pMultiSourceFrameReader->AcquireLatestFrame( &pMultiSourceFrame );

		if (EnableDepth && SUCCEEDED( hr ))
		{
			IDepthFrameReference* pDepthFrameReference = NULL;
			hr = pMultiSourceFrame->get_DepthFrameReference( &pDepthFrameReference );
			if (SUCCEEDED( hr )) hr = pDepthFrameReference->get_RelativeTime( &DepTimeStamp );
			if (SUCCEEDED( hr )) hr = pDepthFrameReference->AcquireFrame( &pDepthFrame );
			SafeRelease( pDepthFrameReference );
		}

		if (EnableColor && SUCCEEDED( hr ))
		{
			IColorFrameReference* pColorFrameReference = NULL;
			hr = pMultiSourceFrame->get_ColorFrameReference( &pColorFrameReference );
			if (SUCCEEDED( hr )) hr = pColorFrameReference->get_RelativeTime( &ColTimeStamp );
			if (SUCCEEDED( hr )) hr = pColorFrameReference->AcquireFrame( &pColorFrame );
			SafeRelease( pColorFrameReference );
		}

		if (EnableInfrared && SUCCEEDED( hr ))
		{
			IInfraredFrameReference* pInfraredFrameReference = NULL;
			hr = pMultiSourceFrame->get_InfraredFrameReference( &pInfraredFrameReference );
			if (SUCCEEDED( hr )) hr = pInfraredFrameReference->get_RelativeTime( &InfTimeStamp );
			if (SUCCEEDED( hr )) hr = pInfraredFrameReference->AcquireFrame( &pInfraredFrame );
		}

		if (SUCCEEDED( hr ))
		{
			IFrameDescription* pDepthFrameDescription = NULL;
			int nDepthWidth = 0;
			int nDepthHeight = 0;

			IFrameDescription* pColorFrameDescription = NULL;
			int nColorWidth = 0;
			int nColorHeight = 0;

			IFrameDescription* pInfraredFrameDescription = NULL;
			int nInfraredWidth = 0;
			int nInfraredHeight = 0;

			if (EnableDepth) {
				// get depth frame data
				if (SUCCEEDED( hr )) hr = pDepthFrame->get_FrameDescription( &pDepthFrameDescription );
				if (SUCCEEDED( hr )) hr = pDepthFrameDescription->get_Width( &nDepthWidth );
				if (SUCCEEDED( hr )) hr = pDepthFrameDescription->get_Height( &nDepthHeight );
				assert( nDepthWidth == _DepthWidth ); assert( nDepthHeight == _DepthHeight );
				size_t bufferSize = nDepthHeight*nDepthWidth * sizeof( uint16_t );
				FrameData& CurFrame = _pDepthFrame[_WritingIdx];
				CurFrame.Size = bufferSize;
				CurFrame.CaptureTimeStamp = DepTimeStamp;
				CurFrame.Width = nDepthWidth;
				CurFrame.Height = nDepthHeight;
				if (CurFrame.pData == nullptr)
					CurFrame.pData = (uint8_t*)std::malloc( bufferSize );
				if (SUCCEEDED( hr )) hr = pDepthFrame->CopyFrameDataToArray( (UINT)(bufferSize / 2), reinterpret_cast<UINT16*>(CurFrame.pData) );
				SafeRelease( pDepthFrameDescription );
			}
			if (EnableColor) {
				// get color frame data
				if (SUCCEEDED( hr )) hr = pColorFrame->get_FrameDescription( &pColorFrameDescription );
				//if (SUCCEEDED( hr )) hr = pColorFrame->CreateFrameDescription( ColorImageFormat_Rgba, &pColorFrameDescription );
				if (SUCCEEDED( hr )) hr = pColorFrameDescription->get_Width( &nColorWidth );
				if (SUCCEEDED( hr )) hr = pColorFrameDescription->get_Height( &nColorHeight );
				assert( nColorWidth == _ColorWidh ); assert( nColorHeight == _ColorHeight );
				size_t bufferSize = nColorHeight*nColorWidth * 4 * sizeof( uint8_t );
				FrameData& CurFrame = _pColorFrame[_WritingIdx];
				CurFrame.Size = bufferSize;
				CurFrame.CaptureTimeStamp = ColTimeStamp;
				CurFrame.Width = nColorWidth;
				CurFrame.Height = nColorHeight;
				if (CurFrame.pData == nullptr)
					CurFrame.pData = (uint8_t*)std::malloc( bufferSize );
				if (SUCCEEDED( hr )) hr = pColorFrame->CopyConvertedFrameDataToArray( bufferSize, reinterpret_cast<BYTE*>(CurFrame.pData), ColorImageFormat_Rgba );
				SafeRelease( pColorFrameDescription );
			}
			if (EnableInfrared) {
				// get Infrared frame data
				if (SUCCEEDED( hr )) hr = pInfraredFrame->get_FrameDescription( &pInfraredFrameDescription );
				if (SUCCEEDED( hr )) hr = pInfraredFrameDescription->get_Width( &nInfraredWidth );
				if (SUCCEEDED( hr )) hr = pInfraredFrameDescription->get_Height( &nInfraredHeight );
				assert( nInfraredWidth== _InfraredWidth); assert( nInfraredHeight== _InfraredHeight);
				size_t bufferSize = nInfraredHeight * nInfraredWidth * sizeof( uint16_t );
				FrameData& CurFrame = _pInfraredFrame[_WritingIdx];
				CurFrame.Size = bufferSize;
				CurFrame.CaptureTimeStamp = InfTimeStamp;
				CurFrame.Width = nInfraredWidth;
				CurFrame.Height = nInfraredHeight;
				if (CurFrame.pData == nullptr)
					CurFrame.pData = (uint8_t*)std::malloc( bufferSize );
				if (SUCCEEDED( hr )) hr = pInfraredFrame->CopyFrameDataToArray( (UINT)(bufferSize / 2), reinterpret_cast<UINT16*>(CurFrame.pData) );
				SafeRelease( pInfraredFrameDescription );
			}
			if (SUCCEEDED( hr ))
			{
				_LatestReadableIdx.store( _WritingIdx, memory_order_release );
				_WritingIdx = (_WritingIdx + 1) % STREAM_BUFFER_COUNT;
				while (_ReadingIdx.load( memory_order_acquire ) == _WritingIdx)
				{
					std::this_thread::yield();
					if (_Streaming.load( memory_order_consume ) == false)
						return;
				}
			}
		}

		SafeRelease( pDepthFrame );
		SafeRelease( pColorFrame );
		SafeRelease( pInfraredFrame );
		SafeRelease( pMultiSourceFrame );
	}
}

IRGBDStreamer* StreamFactory::createFromKinect2()
{
	return new Kinect2Sensor();
}