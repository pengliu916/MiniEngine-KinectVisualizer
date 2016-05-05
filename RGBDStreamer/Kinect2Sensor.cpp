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

#pragma comment (lib, "kinect20.lib")

#define __FILENAME__ (wcsrchr (_T(__FILE__), L'\\') ? wcsrchr (_T(__FILE__), L'\\') + 1 : _T(__FILE__))
#if defined(DEBUG) || defined(_DEBUG)
#ifndef V
#define V(x) { hr = (x); if( FAILED(hr) ) { Trace( __FILENAME__, (DWORD)__LINE__, hr, L###x); __debugbreak(); } }
#endif //#ifndef V
#ifndef VRET
#define VRET(x) { hr = (x); if( FAILED(hr) ) { return Trace( __FILENAME__, (DWORD)__LINE__, hr, L###x); __debugbreak(); } }
#endif //#ifndef VRET
#ifndef W
#define W(x) { hr = (x); if( FAILED(hr) ) { Trace( __FILENAME__, (DWORD)__LINE__, hr, L###x); } }
#endif //#ifndef W
#ifndef WRET
#define WRET(x) { hr = (x); if( FAILED(hr) ) { return Trace( __FILENAME__, (DWORD)__LINE__, hr, L###x); } }
#endif //#ifndef WRET
#else
#ifndef V
#define V(x)           { hr = (x); }
#endif //#ifndef V
#ifndef VRET
#define VRET(x)           { hr = (x); if( FAILED(hr) ) { return hr; } }
#endif //#ifndef VRET
#ifndef W
#define W(x)           { hr = (x); }
#endif //#ifndef W
#ifndef WRET
#define WRET(x)           { hr = (x); if( FAILED(hr) ) { return hr; } }
#endif //#ifndef WRET
#endif //#if defined(DEBUG) || defined(_DEBUG)

inline HRESULT Trace( const wchar_t* strFile, DWORD dwLine, HRESULT hr, const wchar_t* strMsg )
{
	wchar_t szBuffer[512];
	int offset = 0;
	if (strFile) offset += wsprintf( szBuffer, L"line %u in file %s\n", dwLine, strFile );

	offset += wsprintf( szBuffer + offset, L"Calling: %s failed!\n ", strMsg );
	_com_error err( hr );
	wsprintf( szBuffer + offset, err.ErrorMessage() );
	OutputDebugString( szBuffer );
	return hr;
}

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
	HRESULT hr = S_OK;
	VRET( GetDefaultKinectSensor( &_pKinect2Sensor ) );

	int SourceType = FrameSourceTypes_None;

	if (_ColorEnabled) SourceType |= FrameSourceTypes_Color;
	if (_DepthEnabled) SourceType |= FrameSourceTypes_Depth;
	if (_InfraredEnabled) SourceType |= FrameSourceTypes_Infrared;

	VRET( _pKinect2Sensor->Open() );
	VRET( _pKinect2Sensor->OpenMultiSourceFrameReader( SourceType, &_pMultiSourceFrameReader ) );
	VRET( _pMultiSourceFrameReader->SubscribeMultiSourceFrameArrived( &_hFrameArrivalEvent ) );

	return hr;
}

void Kinect2Sensor::Shutdown()
{
	_pMultiSourceFrameReader->UnsubscribeMultiSourceFrameArrived( _hFrameArrivalEvent );
	SafeRelease( _pMultiSourceFrameReader );

	if (_pKinect2Sensor)
	{
		_pKinect2Sensor->Close();
		_pKinect2Sensor->Release();
		_pKinect2Sensor = NULL;
	}
}

Kinect2Sensor::Kinect2Sensor( bool EnableColor, bool EnableDepth, bool EnableInfrared )
{
	HRESULT hr = S_OK;
	_DepthEnabled = EnableDepth;
	_ColorEnabled = EnableColor;
	_InfraredEnabled = EnableInfrared;
	_Streaming.store( false, memory_order_relaxed );
	_hFrameArrivalEvent = (WAITABLE_HANDLE)CreateEvent( NULL, FALSE, FALSE, NULL );
	V( Initialize() );
	return;
}

Kinect2Sensor::~Kinect2Sensor()
{
	_Streaming.store( false, memory_order_relaxed );
	if (_BackGroundThread.joinable())
		_BackGroundThread.join();

	Shutdown();
	CloseHandle( (HANDLE)_hFrameArrivalEvent );
}

void Kinect2Sensor::GetColorReso( uint16_t& Width, uint16_t& Height ) const
{
	Width = _ColorWidth; Height = _ColorHeight;
}

void Kinect2Sensor::GetDepthReso( uint16_t& Width, uint16_t& Height ) const
{
	Width = _DepthWidth; Height = _DepthHeight;
}

void Kinect2Sensor::GetInfraredReso( uint16_t& Width, uint16_t& Height ) const
{
	Width = _InfraredWidth; Height = _InfraredHeight;
}

void Kinect2Sensor::StartStream()
{
	if (!_Streaming.load( memory_order_relaxed ))
	{
		_Streaming.store( true, memory_order_release );
		thread t( &Kinect2Sensor::FrameAcquireLoop, this );
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

void Kinect2Sensor::FrameAcquireLoop()
{
	SetThreadName( "KinectBackground Thread" );

	INT64 ColTimeStampPreFrame = 0;
	INT64 DepTimeStampPreFrame = 0;
	INT64 InfTimeStampPreFrame = 0;

	HRESULT hr = S_OK;
	while (_Streaming.load( memory_order_consume ))
	{
		while (FAILED( hr ))
		{
			Shutdown();
			this_thread::sleep_for( 1s );
			hr = Initialize();
		}
		int idx = WaitForSingleObject( reinterpret_cast<HANDLE>(_hFrameArrivalEvent), 3000 );
		switch (idx)
		{
		case WAIT_TIMEOUT:
			hr = E_FAIL;
			OutputDebugString( L"Wait Kinet Frame Timeout\n" );
			continue;
		case WAIT_OBJECT_0:
			IMultiSourceFrameArrivedEventArgs *pFrameArgs = nullptr;
			V( _pMultiSourceFrameReader->GetMultiSourceFrameArrivedEventData( _hFrameArrivalEvent, &pFrameArgs ) );
			ProcessingFrames( pFrameArgs );
			pFrameArgs->Release();
		}
	}
}

HRESULT Kinect2Sensor::ProcessingFrames( IMultiSourceFrameArrivedEventArgs* pArgs )
{
	HRESULT hr;
	IMultiSourceFrameReference *pFrameReference = nullptr;

	VRET( pArgs->get_FrameReference( &pFrameReference ) );

	IMultiSourceFrame *pFrame = nullptr;
	VRET( pFrameReference->AcquireFrame( &pFrame ) );
	if (_DepthEnabled)
	{
		IDepthFrameReference* pDepthFrameReference = nullptr;
		VRET( pFrame->get_DepthFrameReference( &pDepthFrameReference ) );
		W( ProcessDepthFrame( pDepthFrameReference ) );
		SafeRelease( pDepthFrameReference );
	}
	if (_ColorEnabled)
	{
		IColorFrameReference* pColorFrameReference = nullptr;
		VRET( pFrame->get_ColorFrameReference( &pColorFrameReference ) );
		W( ProcessColorFrame( pColorFrameReference ) );
		SafeRelease( pColorFrameReference );
	}
	if (_InfraredEnabled)
	{
		IInfraredFrameReference* pInfraredFrameReference = nullptr;
		VRET( pFrame->get_InfraredFrameReference( &pInfraredFrameReference ) );
		W( ProcessInfraredFrame( pInfraredFrameReference ) );
		SafeRelease( pInfraredFrameReference );
	}
	pFrameReference->Release();

	if (SUCCEEDED( hr ))
	{
		_LatestReadableIdx.store( _WritingIdx, memory_order_release );
		_WritingIdx = (_WritingIdx + 1) % STREAM_BUFFER_COUNT;
		while (_ReadingIdx.load( memory_order_acquire ) == _WritingIdx)
		{
			std::this_thread::yield();
			if (_Streaming.load( memory_order_consume ) == false)
				return hr;
		}
	}
}

HRESULT Kinect2Sensor::ProcessDepthFrame( IDepthFrameReference* pDepthFrameRef )
{
	IFrameDescription* pDepthFrameDescription = NULL;
	INT64 DepTimeStamp = 0;
	int nDepthWidth = 0;
	int nDepthHeight = 0;
	IDepthFrame* pDepthFrame = NULL;
	HRESULT hr = S_OK;
	WRET( pDepthFrameRef->AcquireFrame( &pDepthFrame ) );
	V( pDepthFrameRef->get_RelativeTime( &DepTimeStamp ) );
	// get depth frame data
	V( pDepthFrame->get_FrameDescription( &pDepthFrameDescription ) );
	V( pDepthFrameDescription->get_Width( &nDepthWidth ) );
	V( pDepthFrameDescription->get_Height( &nDepthHeight ) );
	assert( nDepthWidth == _DepthWidth ); assert( nDepthHeight == _DepthHeight );
	size_t bufferSize = nDepthHeight*nDepthWidth * sizeof( uint16_t );
	FrameData& CurFrame = _pDepthFrame[_WritingIdx];
	CurFrame.Size = bufferSize;
	CurFrame.CaptureTimeStamp = DepTimeStamp;
	CurFrame.Width = nDepthWidth;
	CurFrame.Height = nDepthHeight;
	if (CurFrame.pData == nullptr)
		CurFrame.pData = (uint8_t*)std::malloc( bufferSize );
	V( pDepthFrame->CopyFrameDataToArray( (UINT)(bufferSize / 2), reinterpret_cast<UINT16*>(CurFrame.pData) ) );
	SafeRelease( pDepthFrameDescription );
	SafeRelease( pDepthFrame );
	return hr;
}

HRESULT Kinect2Sensor::ProcessColorFrame( IColorFrameReference* pColorFrameRef )
{
	IFrameDescription* pColorFrameDescription = NULL;
	INT64 ColTimeStamp = 0;
	int nColorWidth = 0;
	int nColorHeight = 0;
	IColorFrame* pColorFrame = NULL;
	HRESULT hr = S_OK;
	WRET( pColorFrameRef->AcquireFrame( &pColorFrame ) );
	V( pColorFrameRef->get_RelativeTime( &ColTimeStamp ) );
	// get color frame data
	V( pColorFrame->get_FrameDescription( &pColorFrameDescription ) );
	V( pColorFrameDescription->get_Width( &nColorWidth ) );
	V( pColorFrameDescription->get_Height( &nColorHeight ) );
	assert( nColorWidth == _ColorWidth ); assert( nColorHeight == _ColorHeight );
	size_t bufferSize = nColorHeight*nColorWidth * 4 * sizeof( uint8_t );
	FrameData& CurFrame = _pColorFrame[_WritingIdx];
	CurFrame.Size = bufferSize;
	CurFrame.CaptureTimeStamp = ColTimeStamp;
	CurFrame.Width = nColorWidth;
	CurFrame.Height = nColorHeight;
	if (CurFrame.pData == nullptr)
		CurFrame.pData = (uint8_t*)std::malloc( bufferSize );
	V( pColorFrame->CopyConvertedFrameDataToArray( bufferSize, reinterpret_cast<BYTE*>(CurFrame.pData), ColorImageFormat_Rgba ) );
	SafeRelease( pColorFrameDescription );
	SafeRelease( pColorFrame );
	return hr;
}

HRESULT Kinect2Sensor::ProcessInfraredFrame( IInfraredFrameReference* pInfraredFrameRef )
{
	IFrameDescription* pInfraredFrameDescription = NULL;
	INT64 InfTimeStamp = 0;
	int nInfraredWidth = 0;
	int nInfraredHeight = 0;
	IInfraredFrame* pInfraredFrame = NULL;
	HRESULT hr = S_OK;
	VRET( pInfraredFrameRef->AcquireFrame( &pInfraredFrame ) );
	V( pInfraredFrameRef->get_RelativeTime( &InfTimeStamp ) );
	// get Infrared frame data
	V( pInfraredFrame->get_FrameDescription( &pInfraredFrameDescription ) );
	V( pInfraredFrameDescription->get_Width( &nInfraredWidth ) );
	V( pInfraredFrameDescription->get_Height( &nInfraredHeight ) );
	assert( nInfraredWidth == _InfraredWidth ); assert( nInfraredHeight == _InfraredHeight );
	size_t bufferSize = nInfraredHeight * nInfraredWidth * sizeof( uint16_t );
	FrameData& CurFrame = _pInfraredFrame[_WritingIdx];
	CurFrame.Size = bufferSize;
	CurFrame.CaptureTimeStamp = InfTimeStamp;
	CurFrame.Width = nInfraredWidth;
	CurFrame.Height = nInfraredHeight;
	if (CurFrame.pData == nullptr)
		CurFrame.pData = (uint8_t*)std::malloc( bufferSize );
	V( pInfraredFrame->CopyFrameDataToArray( (UINT)(bufferSize / 2), reinterpret_cast<UINT16*>(CurFrame.pData) ) );
	SafeRelease( pInfraredFrameDescription );
	SafeRelease( pInfraredFrame );
	return hr;
}

IRGBDStreamer* StreamFactory::createFromKinect2( bool EnableColor, bool EnableDepth, bool EnableInfrared )
{
	return new Kinect2Sensor( EnableColor, EnableDepth, EnableInfrared );
}