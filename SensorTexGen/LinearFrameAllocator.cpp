#include "pch.h"
#include "LinearFrameAllocator.h"

using namespace std;

//-----------------------------------------------------------------------------
// KinectBuffer
//-----------------------------------------------------------------------------
KinectBuffer::KinectBuffer(DXGI_FORMAT format, uint32_t numElements, 
    uint32_t elementSize)
    : _elementCount(numElements), 
      _elementSize(elementSize), 
      _dataFormat(format)
{
    _bufferSize = numElements * elementSize;
    // [NOTE] Upload heap resource can't have many resource flags
    _resourceFlags = D3D12_RESOURCE_FLAG_NONE;
    _uav.ptr = ~0ull;
    _srv.ptr = ~0ull;
    Create(L"KinectBuffer");
}

KinectBuffer::~KinectBuffer()
{
    Destroy();
}

void
KinectBuffer::Create(const std::wstring& name)
{
    D3D12_RESOURCE_DESC resourceDesc = DescribeBuffer();

    // Upload heap resource could only be this state
    m_UsageState = D3D12_RESOURCE_STATE_GENERIC_READ;

    D3D12_HEAP_PROPERTIES heapProps;
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
    heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heapProps.CreationNodeMask = 1;
    heapProps.VisibleNodeMask = 1;

    HRESULT hr;
    V(Graphics::g_device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc, 
        m_UsageState, nullptr, IID_PPV_ARGS(&m_pResource)));

    m_GpuVirtualAddress = m_pResource->GetGPUVirtualAddress();

#ifdef RELEASE
    (name);
#else
    m_pResource->SetName(name.c_str());
#endif

    CreateDerivedViews();

    m_pResource->Map(0, nullptr, &_cpuVirtualAddr);
}

void
KinectBuffer::Destroy()
{
    m_pResource->Unmap(0, nullptr);
    GpuResource::Destroy();
}

D3D12_RESOURCE_DESC
KinectBuffer::DescribeBuffer()
{
    ASSERT(_bufferSize != 0);
    D3D12_RESOURCE_DESC desc = {};
    desc.Alignment = 0;
    desc.DepthOrArraySize = 1;
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Flags = _resourceFlags;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.Height = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Width = (UINT64)_bufferSize;
    return desc;
}

void
KinectBuffer::CreateDerivedViews()
{
    // [NOTE] Resource in upload heap can't have UAV
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Format = _dataFormat;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Buffer.NumElements = _elementCount;
    srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

    if (_srv.ptr == ~0ull)
        _srv = Graphics::g_pCSUDescriptorHeap->Append().GetCPUHandle();
    Graphics::g_device->CreateShaderResourceView(
        m_pResource.Get(), &srvDesc, _srv);
}

//-----------------------------------------------------------------------------
// LinearFrameAllocator
//-----------------------------------------------------------------------------
LinearFrameAllocator::LinearFrameAllocator(
    uint32_t numElements, uint32_t elementSize, DXGI_FORMAT format)
    : _numElements(numElements), 
      _elementSize(elementSize), 
      _bufferFormat(format)
{
}

LinearFrameAllocator::~LinearFrameAllocator()
{
}

KinectBuffer*
LinearFrameAllocator::RequestFrameBuffer()
{
    while (!_retiredBuffers.empty() && 
        Graphics::g_cmdListMngr.IsFenceComplete(
            _retiredBuffers.front().first)) {
        _availableBuffers.push(_retiredBuffers.front().second);
        _retiredBuffers.pop();
    }
    KinectBuffer* bufferPtr = nullptr;
    if (!_availableBuffers.empty()) {
        bufferPtr = _availableBuffers.front();
        _availableBuffers.pop();
    } else {
        bufferPtr = CreateNewFrameBuffer();
        _bufferPoll.emplace_back(bufferPtr);
    }
    return bufferPtr;
}

void
LinearFrameAllocator::DiscardBuffer(
    uint64_t fenceID, KinectBuffer* buffer)
{
    _retiredBuffers.push(make_pair(fenceID, buffer));
}

void
LinearFrameAllocator::Destory()
{
    _bufferPoll.clear();
}

KinectBuffer*
LinearFrameAllocator::CreateNewFrameBuffer()
{
    KinectBuffer* bufferPtr = 
        new KinectBuffer(_bufferFormat, _numElements, _elementSize);
    return bufferPtr;
}