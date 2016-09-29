#pragma once
#include "GpuResource.h"

//-----------------------------------------------------------------------------
// KinectBuffer
//-----------------------------------------------------------------------------
class KinectBuffer : public GpuResource
{
public:
    KinectBuffer(
        DXGI_FORMAT format, uint32_t numElements, uint32_t elementSize);
    ~KinectBuffer();
    void* GetMappedPtr() const { return _cpuVirtualAddr; }
    const D3D12_CPU_DESCRIPTOR_HANDLE& GetSRV() const { return _srv; }

protected:
    void Create(const std::wstring& name);
    void Destroy();
    D3D12_RESOURCE_DESC DescribeBuffer();
    void CreateDerivedViews();

private:
    D3D12_CPU_DESCRIPTOR_HANDLE _uav;
    D3D12_CPU_DESCRIPTOR_HANDLE _srv;

    size_t _bufferSize;
    uint32_t _elementCount;
    uint32_t _elementSize;
    D3D12_RESOURCE_FLAGS _resourceFlags;
    DXGI_FORMAT _dataFormat;
    void* _cpuVirtualAddr;

    const D3D12_CPU_DESCRIPTOR_HANDLE& GetUAV() const { return _uav; }
};

//-----------------------------------------------------------------------------
// LinearFrameAllocator
//-----------------------------------------------------------------------------
class LinearFrameAllocator
{
public:
    LinearFrameAllocator(
        uint32_t numElements, uint32_t elementSize, DXGI_FORMAT format);
    ~LinearFrameAllocator();

    KinectBuffer* RequestFrameBuffer();
    void DiscardBuffer(uint64_t fenceID, KinectBuffer* buffer);
    void Destory();

private:
    KinectBuffer* CreateNewFrameBuffer();

    uint32_t _numElements;
    uint32_t _elementSize;

    DXGI_FORMAT _bufferFormat;

    std::vector<std::unique_ptr<KinectBuffer>> _bufferPoll;
    std::queue<std::pair<uint64_t, KinectBuffer*>> _retiredBuffers;
    std::queue<KinectBuffer*> _availableBuffers;
};