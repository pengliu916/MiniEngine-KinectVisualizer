#include "LibraryHeader.h"
#include "Graphics.h"
#include "CmdListMngr.h"
#include "Utility.h"

using namespace std;
using namespace Microsoft::WRL;

#include "LinearAllocator.h"

LinearAllocatorPageMngr LinearAllocator::sm_PageMngr[2] = {
    LinearAllocatorPageMngr(kGpuExclusive),
    LinearAllocatorPageMngr(kCpuWritable)
};

//------------------------------------------------------------------------------
// LinearAllocatorPageMngr
//------------------------------------------------------------------------------
LinearAllocatorPageMngr::LinearAllocatorPageMngr(LinearAllocatorType Type)
{
    InitializeCriticalSection(&m_CS);
    m_AllocationType = Type;
}

LinearAllocatorPageMngr::~LinearAllocatorPageMngr()
{
    DeleteCriticalSection(&m_CS);
}

LinearAllocationPage*
LinearAllocatorPageMngr::RequestPage()
{
    CriticalSectionScope LockGard(&m_CS);
    while (!m_RetiredPages.empty() &&
        Graphics::g_cmdListMngr.IsFenceComplete(m_RetiredPages.front().first)) {
        m_AvailablePages.push(m_RetiredPages.front().second);
        m_RetiredPages.pop();
    }
    LinearAllocationPage* PagePtr = nullptr;
    if (!m_AvailablePages.empty()) {
        PagePtr = m_AvailablePages.front();
        m_AvailablePages.pop();
    } else {
        PagePtr = CreateNewPage();
        m_PagePool.emplace_back(PagePtr);
    }
    return PagePtr;
}

void
LinearAllocatorPageMngr::DiscardPages(
    uint64_t FenceValue, const vector<LinearAllocationPage*>& UsedPages)
{
    CriticalSectionScope LockGard(&m_CS);
    for (auto iter = UsedPages.begin(); iter != UsedPages.end(); ++iter) {
        m_RetiredPages.push(make_pair(FenceValue, *iter));
    }
}

void
LinearAllocatorPageMngr::FreeLargePages(
    uint64_t FenceValue, const vector<LinearAllocationPage*>& LargePages)
{
    CriticalSectionScope LockGard(&m_CS);

    while (!m_DeletionQueue.empty() &&
        Graphics::g_cmdListMngr.IsFenceComplete(
            m_DeletionQueue.front().first)) {
        delete m_DeletionQueue.front().second;
        m_DeletionQueue.pop();
    }

    for (auto iter = LargePages.begin(); iter != LargePages.end(); ++iter) {
        (*iter)->Unmap();
        m_DeletionQueue.push(make_pair(FenceValue, *iter));
    }
}

LinearAllocationPage*
LinearAllocatorPageMngr::CreateNewPage(size_t PageSize)
{
    HRESULT hr;
    D3D12_HEAP_PROPERTIES HeapProps;
    HeapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    HeapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    HeapProps.CreationNodeMask = 1;
    HeapProps.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC ResourceDesc;
    ResourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    ResourceDesc.Alignment = 0;
    ResourceDesc.Height = 1;
    ResourceDesc.DepthOrArraySize = 1;
    ResourceDesc.MipLevels = 1;
    ResourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    ResourceDesc.SampleDesc.Count = 1;
    ResourceDesc.SampleDesc.Quality = 0;
    ResourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12_RESOURCE_STATES DefaultUsage;

    if (m_AllocationType == kGpuExclusive)
    {
        HeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
        ResourceDesc.Width = PageSize == 0 ? kGpuAllocatorPageSize : PageSize;
        ResourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        DefaultUsage = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    } else
    {
        HeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        ResourceDesc.Width = PageSize == 0 ? kCpuAllocatorPageSize : PageSize;
        ResourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
        DefaultUsage = D3D12_RESOURCE_STATE_GENERIC_READ;
    }

    ID3D12Resource* pBuffer;
    V(Graphics::g_device->CreateCommittedResource(&HeapProps,
        D3D12_HEAP_FLAG_NONE, &ResourceDesc, DefaultUsage,
        nullptr, IID_PPV_ARGS(&pBuffer)));

    pBuffer->SetName(L"LinearAllocator Page");

    return new LinearAllocationPage(pBuffer, DefaultUsage);
}

void
LinearAllocatorPageMngr::Destory()
{
    m_PagePool.clear();
}

//------------------------------------------------------------------------------
// LinearAllocator
//------------------------------------------------------------------------------
LinearAllocator::LinearAllocator(LinearAllocatorType Type)
    : m_AllocationType(Type), m_PageSize(0),
    m_CurOffset(~0ull), m_CurPage(nullptr)
{
    ASSERT(Type > kInvalidAllocator && Type < kNumAllocatorTypes);
    m_PageSize =
        (Type == kGpuExclusive ? kGpuAllocatorPageSize : kCpuAllocatorPageSize);
}

DynAlloc
LinearAllocator::Allocate(size_t SizeInByte, size_t Alignment)
{
    const size_t AlignmentMask = Alignment - 1;
    // Assert that it's a power of two.
    ASSERT((AlignmentMask & Alignment) == 0);
    const size_t AlignedSize = AlignUpWithMask(SizeInByte, AlignmentMask);

    if (AlignedSize > m_PageSize)
        return AllocateLargePage(AlignedSize);

    m_CurOffset = AlignUp(m_CurOffset, Alignment);
    if (m_CurOffset + AlignedSize > m_PageSize) {
        ASSERT(m_CurPage != nullptr);
        m_RetiredPages.push_back(m_CurPage);
        m_CurPage = nullptr;
    }
    if (m_CurPage == nullptr) {
        m_CurPage = sm_PageMngr[m_AllocationType].RequestPage();
        m_CurOffset = 0;
    }

    DynAlloc ret(*m_CurPage, m_CurOffset, AlignedSize);
    ret.GpuAddress = m_CurPage->m_GpuVirtualAddr + m_CurOffset;
    ret.DataPtr = (uint8_t*)m_CurPage->m_CpuVirtualAddr + m_CurOffset;

    m_CurOffset += AlignedSize;

    return ret;
}

void
LinearAllocator::CleanupUsedPages(uint64_t FenceID)
{
    if (m_CurPage == nullptr) {
        return;
    }
    m_RetiredPages.push_back(m_CurPage);
    m_CurPage = nullptr;
    m_CurOffset = 0;

    sm_PageMngr[m_AllocationType].DiscardPages(FenceID, m_RetiredPages);
    m_RetiredPages.clear();

    sm_PageMngr[m_AllocationType].FreeLargePages(FenceID, m_LargePageList);
    m_LargePageList.clear();
}

void
LinearAllocator::DestroyAll()
{
    sm_PageMngr[0].Destory();
    sm_PageMngr[1].Destory();
}


DynAlloc
LinearAllocator::AllocateLargePage(size_t SizeInBytes)
{
    LinearAllocationPage* OneOff =
        sm_PageMngr[m_AllocationType].CreateNewPage(SizeInBytes);
    m_LargePageList.push_back(OneOff);

    DynAlloc ret(*OneOff, 0, SizeInBytes);
    ret.DataPtr = OneOff->m_CpuVirtualAddr;
    ret.GpuAddress = OneOff->m_GpuVirtualAddr;

    return ret;
}