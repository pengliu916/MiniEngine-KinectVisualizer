#include "pch.h"
#include "LinearFrameAllocator.h"

using namespace std;

//-----------------------------------------------------------------------------
// KinectBuffer
//-----------------------------------------------------------------------------
KinectBuffer::KinectBuffer(DXGI_FORMAT Format, uint32_t NumElements, 
    uint32_t ElementSize)
    : m_ElementCount(NumElements), 
      m_ElementSize(ElementSize), 
      m_DataFormat(Format)
{
    m_BufferSize = NumElements * ElementSize;
    // [NOTE] Upload heap resource can't have many resource flags
    m_ResourceFlags = D3D12_RESOURCE_FLAG_NONE;
    m_UAV.ptr = ~0ull;
    m_SRV.ptr = ~0ull;
    Create(L"KinectBuffer");
}

KinectBuffer::~KinectBuffer()
{
    Destroy();
}

void KinectBuffer::Create(const std::wstring& Name)
{
    D3D12_RESOURCE_DESC ResourceDesc = DescribeBuffer();

    // Upload heap resource could only be this state
    m_UsageState = D3D12_RESOURCE_STATE_GENERIC_READ;

    D3D12_HEAP_PROPERTIES HeapProps;
    HeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
    HeapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    HeapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    HeapProps.CreationNodeMask = 1;
    HeapProps.VisibleNodeMask = 1;

    HRESULT hr;
    V(Graphics::g_device->CreateCommittedResource(
        &HeapProps, D3D12_HEAP_FLAG_NONE, &ResourceDesc, 
        m_UsageState, nullptr, IID_PPV_ARGS(&m_pResource)));

    m_GpuVirtualAddress = m_pResource->GetGPUVirtualAddress();

#ifdef RELEASE
    (Name);
#else
    m_pResource->SetName(Name.c_str());
#endif

    CreateDerivedViews();

    m_pResource->Map(0, nullptr, &m_CpuVirtualAddr);
}

void KinectBuffer::Destroy()
{
    m_pResource->Unmap(0, nullptr);
    GpuResource::Destroy();
}

D3D12_RESOURCE_DESC KinectBuffer::DescribeBuffer()
{
    ASSERT(m_BufferSize != 0);
    D3D12_RESOURCE_DESC Desc = {};
    Desc.Alignment = 0;
    Desc.DepthOrArraySize = 1;
    Desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    Desc.Flags = m_ResourceFlags;
    Desc.Format = DXGI_FORMAT_UNKNOWN;
    Desc.Height = 1;
    Desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Desc.MipLevels = 1;
    Desc.SampleDesc.Count = 1;
    Desc.SampleDesc.Quality = 0;
    Desc.Width = (UINT64)m_BufferSize;
    return Desc;
}

void KinectBuffer::CreateDerivedViews()
{
    // [NOTE] Resource in upload heap can't have UAV
    D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc = {};
    SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    SRVDesc.Format = m_DataFormat;
    SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    SRVDesc.Buffer.NumElements = m_ElementCount;
    SRVDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

    if (m_SRV.ptr == ~0ull)
        m_SRV = Graphics::g_pCSUDescriptorHeap->Append().GetCPUHandle();
    Graphics::g_device->CreateShaderResourceView(
        m_pResource.Get(), &SRVDesc, m_SRV);
}

//-----------------------------------------------------------------------------
// LinearFrameAllocator
//-----------------------------------------------------------------------------
LinearFrameAllocator::LinearFrameAllocator(
    uint32_t NumElements, uint32_t ElementSize, DXGI_FORMAT Format)
    : m_NumElements(NumElements), 
      m_ElementSize(ElementSize), 
      m_BufferFormat(Format)
{
}

LinearFrameAllocator::~LinearFrameAllocator()
{
}

KinectBuffer* LinearFrameAllocator::RequestFrameBuffer()
{
    while (!m_RetiredBuffers.empty() && 
        Graphics::g_cmdListMngr.IsFenceComplete(
            m_RetiredBuffers.front().first)) {
        m_AvailableBuffers.push(m_RetiredBuffers.front().second);
        m_RetiredBuffers.pop();
    }
    KinectBuffer* BufferPtr = nullptr;
    if (!m_AvailableBuffers.empty()) {
        BufferPtr = m_AvailableBuffers.front();
        m_AvailableBuffers.pop();
    } else {
        BufferPtr = CreateNewFrameBuffer();
        m_BufferPoll.emplace_back(BufferPtr);
    }
    return BufferPtr;
}

void LinearFrameAllocator::DiscardBuffer(
    uint64_t FenceID, KinectBuffer* Buffer)
{
    m_RetiredBuffers.push(make_pair(FenceID, Buffer));
}

void LinearFrameAllocator::Destory()
{
    m_BufferPoll.clear();
}

KinectBuffer* LinearFrameAllocator::CreateNewFrameBuffer()
{
    KinectBuffer* BufferPtr = 
        new KinectBuffer(m_BufferFormat, m_NumElements, m_ElementSize);
    return BufferPtr;
}