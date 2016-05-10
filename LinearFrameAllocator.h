#pragma once
#include "GpuResource.h"

//--------------------------------------------------------------------------------------
// KinectBuffer
//--------------------------------------------------------------------------------------
class KinectBuffer : public GpuResource
{
public:
	KinectBuffer( DXGI_FORMAT Format, uint32_t NumElements, uint32_t ElementSize );
	~KinectBuffer();
	void* GetMappedPtr() const { return m_CpuVirtualAddr; }
	const D3D12_CPU_DESCRIPTOR_HANDLE& GetSRV() const { return m_SRV; }

protected:
	void Create( const std::wstring& Name );
	void Destroy();
	D3D12_RESOURCE_DESC DescribeBuffer();
	void CreateDerivedViews();

	D3D12_CPU_DESCRIPTOR_HANDLE m_UAV;
	D3D12_CPU_DESCRIPTOR_HANDLE m_SRV;

	size_t m_BufferSize;
	uint32_t m_ElementCount;
	uint32_t m_ElementSize;
	D3D12_RESOURCE_FLAGS m_ResourceFlags;
	DXGI_FORMAT m_DataFormat;

	void* m_CpuVirtualAddr;

private:
	const D3D12_CPU_DESCRIPTOR_HANDLE& GetUAV() const { return m_UAV; }
};

//--------------------------------------------------------------------------------------
// LinearFrameAllocator
//--------------------------------------------------------------------------------------
class LinearFrameAllocator
{
public:
	LinearFrameAllocator( uint32_t NumElements, uint32_t ElementSize, DXGI_FORMAT Format );
	~LinearFrameAllocator();

	KinectBuffer* RequestFrameBuffer();
	void DiscardBuffer( uint64_t FenceID, KinectBuffer* Buffer );
	void Destory();

private:
	KinectBuffer* CreateNewFrameBuffer();

	uint32_t m_NumElements;
	uint32_t m_ElementSize;

	DXGI_FORMAT m_BufferFormat;
	
	std::vector<std::unique_ptr<KinectBuffer>> m_BufferPoll;
	std::queue<std::pair<uint64_t, KinectBuffer*>> m_RetiredBuffers;
	std::queue<KinectBuffer*> m_AvailableBuffers;
};

