#include "LibraryHeader.h"
#include "CommandContext.h"
#include "Graphics.h"
#include "CmdListMngr.h"
#include "Utility.h"
#include "DynamicDescriptorHeap.h"

CRITICAL_SECTION DynamicDescriptorHeap::sm_CS;

std::vector<Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>>
    DynamicDescriptorHeap::sm_DescHeapPool[2];

std::queue<std::pair<uint64_t, ID3D12DescriptorHeap*>>
    DynamicDescriptorHeap::sm_RetiredDescHeaps[2];

std::queue<ID3D12DescriptorHeap*>
    DynamicDescriptorHeap::sm_AvailableDescHeaps[2];

DynamicDescriptorHeap::DynamicDescriptorHeap(CommandContext& OwningCtx,
    D3D12_DESCRIPTOR_HEAP_TYPE HeapType)
    :m_OwningCtx(OwningCtx), m_DescType(HeapType)
{
    m_CurrentHeapPtr = nullptr;
    m_CurrentOffset = 0;
    m_DescSize = Graphics::g_device->GetDescriptorHandleIncrementSize(HeapType);
}

DynamicDescriptorHeap::~DynamicDescriptorHeap()
{
}

void
DynamicDescriptorHeap::Initialize()
{
    InitializeCriticalSection(&sm_CS);
}

void
DynamicDescriptorHeap::Shutdown()
{
    DestroyAll();
    DeleteCriticalSection(&sm_CS);
}

void
DynamicDescriptorHeap::DestroyAll()
{
    sm_DescHeapPool[0].clear();
    sm_DescHeapPool[1].clear();
}

void
DynamicDescriptorHeap::CleanupUsedHeaps(uint64_t FenceValue)
{
    _RetireCurrentHeap();
    _RetireUsedHeaps(FenceValue);
    m_GfxHandleCache._ClearCache();
    m_CptHandleCache._ClearCache();
}

D3D12_GPU_DESCRIPTOR_HANDLE
DynamicDescriptorHeap::UploadDirect(D3D12_CPU_DESCRIPTOR_HANDLE Handles)
{
    if (!_HasSpace(1)) {
        _RetireCurrentHeap();
        _UnbindAllValid();
    }
    m_OwningCtx.SetDescriptorHeap(m_DescType, _GetHeapPointer());
    DescriptorHandle DestHandle =
        m_FirstDesc + m_CurrentOffset * m_DescSize;
    m_CurrentOffset += 1;
    Graphics::g_device->CopyDescriptorsSimple(1, DestHandle.GetCPUHandle(),
        Handles, m_DescType);
    return DestHandle.GetGPUHandle();
}

uint32_t
DynamicDescriptorHeap::DescriptorHandleCache::_ComputeStagedSize()
{
    uint32_t NeededSpace = 0;
    uint32_t RootIndex;
    uint32_t StaleParams = m_StaleRootParamsBitMap;
    while (BitScanForward((unsigned long*)&RootIndex, StaleParams)) {
        StaleParams ^= (1 << RootIndex);
        uint32_t MaxSetHandle;
        ASSERT(TRUE == BitScanReverse((unsigned long*)&MaxSetHandle,
            m_RootDescriptorTable[RootIndex].AssignedHandlesBitMap));
        NeededSpace += MaxSetHandle + 1;
    }
    return NeededSpace;
}

void
DynamicDescriptorHeap::DescriptorHandleCache::_CopyAndBindStaleTables(
    D3D12_DESCRIPTOR_HEAP_TYPE Type, uint32_t DescSize,
    DescriptorHandle DestHandleStart, ID3D12GraphicsCommandList* CmdList,
    void(STDMETHODCALLTYPE ID3D12GraphicsCommandList::*SetFunc)(
        UINT, D3D12_GPU_DESCRIPTOR_HANDLE))
{
    uint32_t StaleParamCount = 0;
    uint32_t TableSize[DescriptorHandleCache::kMaxNumDescriptorTables];
    uint32_t RootIndices[DescriptorHandleCache::kMaxNumDescriptorTables];
    uint32_t RootIndex;

    // Sum the maximum assigned offsets of stale descriptor tables to
    // determine total needed space
    uint32_t StaleParams = m_StaleRootParamsBitMap;
    while (BitScanForward((unsigned long*)&RootIndex, StaleParams)) {
        RootIndices[StaleParamCount] = RootIndex;
        StaleParams ^= (1 << RootIndex);
        uint32_t MaxSetHandle;
        ASSERT(TRUE == BitScanReverse((unsigned long*)&MaxSetHandle,
            m_RootDescriptorTable[RootIndex].AssignedHandlesBitMap));
        TableSize[StaleParamCount] = MaxSetHandle + 1;
        ++StaleParamCount;
    }

    ASSERT(StaleParamCount <= DescriptorHandleCache::kMaxNumDescriptorTables);
    m_StaleRootParamsBitMap = 0;

    static const uint32_t kMaxDescriptorsPerCopy = 16;
    UINT NumDestDescriptorRanges = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE
        pDestDescriptorRangeStarts[kMaxDescriptorsPerCopy];
    UINT pDestDescriptorRangeSizes[kMaxDescriptorsPerCopy];
    
    UINT NumSrcDescriptorRanges = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE
        pSrcDescriptorRangeStarts[kMaxDescriptorsPerCopy];
    UINT pSrcDescriptorRangeSizes[kMaxDescriptorsPerCopy];

    for (uint32_t i = 0; i < StaleParamCount; ++i) {
        RootIndex = RootIndices[i];
        (CmdList->*SetFunc)(RootIndex, DestHandleStart.GetGPUHandle());

        DescriptorTableCache& RootDescTable = m_RootDescriptorTable[RootIndex];
        D3D12_CPU_DESCRIPTOR_HANDLE* SrcHandles = RootDescTable.TableStart;
        uint64_t SetHandles = (uint64_t)RootDescTable.AssignedHandlesBitMap;
        D3D12_CPU_DESCRIPTOR_HANDLE CurDest = DestHandleStart.GetCPUHandle();
        DestHandleStart += TableSize[i] * DescSize;

        unsigned long SkipCount;
        while (BitScanForward64(&SkipCount, SetHandles)) {
            SetHandles >>= SkipCount;
            SrcHandles += SkipCount;
            CurDest.ptr += SkipCount * DescSize;

            unsigned long DescriptorCount;
            BitScanForward64(&DescriptorCount, ~SetHandles);
            SetHandles >>= DescriptorCount;

            // If we run out of temp room, copy what we've got so far
            if (NumSrcDescriptorRanges + DescriptorCount >
                kMaxDescriptorsPerCopy) {
                Graphics::g_device->CopyDescriptors(NumDestDescriptorRanges,
                    pDestDescriptorRangeStarts, pDestDescriptorRangeSizes,
                    NumSrcDescriptorRanges, pSrcDescriptorRangeStarts,
                    pSrcDescriptorRangeSizes, Type);
                NumSrcDescriptorRanges = 0;
                NumDestDescriptorRanges = 0;
            }

            // Setup destination range
            pDestDescriptorRangeStarts[NumDestDescriptorRanges] = CurDest;
            pDestDescriptorRangeSizes[NumDestDescriptorRanges] =
                DescriptorCount;
            ++NumDestDescriptorRanges;

            // Setup source ranges 
            // (one descriptor each because we don't assume they are contiguous)
            for (UINT i = 0; i < DescriptorCount; ++i) {
                pSrcDescriptorRangeStarts[NumSrcDescriptorRanges] =
                    SrcHandles[i];
                pSrcDescriptorRangeSizes[NumSrcDescriptorRanges] = 1;
                ++NumSrcDescriptorRanges;
            }

            // Move the destination pointer forward by the 
            // number of descriptors we will copy
            SrcHandles += DescriptorCount;
            CurDest.ptr += DescriptorCount * DescSize;
        }
    }
    Graphics::g_device->CopyDescriptors(NumDestDescriptorRanges,
        pDestDescriptorRangeStarts, pDestDescriptorRangeSizes,
        NumSrcDescriptorRanges, pSrcDescriptorRangeStarts,
        pSrcDescriptorRangeSizes, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void
DynamicDescriptorHeap::DescriptorHandleCache::_UnbindAllValid()
{
    m_StaleRootParamsBitMap = 0;
    unsigned long TableParams = m_RootDescriptorTablesBitMap;
    unsigned long RootIndex;
    while (BitScanForward(&RootIndex, TableParams)) {
        TableParams ^= (1 << RootIndex);
        if (m_RootDescriptorTable[RootIndex].AssignedHandlesBitMap != 0) {
            m_StaleRootParamsBitMap |= (1 << RootIndex);
        }
    }
}

void
DynamicDescriptorHeap::DescriptorHandleCache::_StageDescriptorHandles(
    UINT RootIndex, UINT Offset, UINT NumHandles,
    const D3D12_CPU_DESCRIPTOR_HANDLE Handles[])
{
    ASSERT(((1 << RootIndex) & m_RootDescriptorTablesBitMap) != 0);
    ASSERT(Offset + NumHandles <= m_RootDescriptorTable[RootIndex].TableSize);

    DescriptorTableCache& TableCache = m_RootDescriptorTable[RootIndex];
    D3D12_CPU_DESCRIPTOR_HANDLE* CopyDest = TableCache.TableStart + Offset;
    for (UINT i = 0; i < NumHandles; ++i) {
        CopyDest[i] = Handles[i];
    }
    TableCache.AssignedHandlesBitMap |= ((1 << NumHandles) - 1) << Offset;
    m_StaleRootParamsBitMap |= (1 << RootIndex);
}

void
DynamicDescriptorHeap::DescriptorHandleCache::_ParseRootSignature(
    D3D12_DESCRIPTOR_HEAP_TYPE Type, const RootSignature& RootSig)
{
    UINT CurrentOffset = 0;
    ASSERT(RootSig.m_NumParameters <= 16);

    m_StaleRootParamsBitMap = 0;
    m_RootDescriptorTablesBitMap = (Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ?
        RootSig.m_SamplerTableBitMap : RootSig.m_DescriptorTableBitMap);

    unsigned long TableParams = m_RootDescriptorTablesBitMap;
    unsigned long RootIndex;
    while (BitScanForward(&RootIndex, TableParams)) {
        TableParams ^= (1 << RootIndex);
        UINT TableSize = RootSig.m_DescriptorTableSize[RootIndex];
        ASSERT(TableSize > 0);

        DescriptorTableCache& RootDescriptorTable =
            m_RootDescriptorTable[RootIndex];
        RootDescriptorTable.AssignedHandlesBitMap = 0;
        RootDescriptorTable.TableStart = m_HandleCache + CurrentOffset;
        RootDescriptorTable.TableSize = TableSize;

        CurrentOffset += TableSize;
    }
    m_MaxCachedDescriptors = CurrentOffset;
    ASSERT(m_MaxCachedDescriptors <= kMaxNumDescriptors);
}

ID3D12DescriptorHeap*
DynamicDescriptorHeap::_RequestDescriptorHeap(
    D3D12_DESCRIPTOR_HEAP_TYPE HeapType)
{
    CriticalSectionScope LockGard(&sm_CS);
    uint32_t idx = HeapType == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ? 1 : 0;
    while (!sm_RetiredDescHeaps[idx].empty() &&
        Graphics::g_cmdListMngr.IsFenceComplete(
            sm_RetiredDescHeaps[idx].front().first)) {
        sm_AvailableDescHeaps[idx].push(
            sm_RetiredDescHeaps[idx].front().second);
        sm_RetiredDescHeaps[idx].pop();
    }
    if (!sm_AvailableDescHeaps[idx].empty()) {
        ID3D12DescriptorHeap* HeapPtr = sm_AvailableDescHeaps[idx].front();
        sm_AvailableDescHeaps[idx].pop();
        return HeapPtr;
    } else {
        D3D12_DESCRIPTOR_HEAP_DESC HeapDesc = {};
        HeapDesc.Type = HeapType;
        HeapDesc.NumDescriptors = kNumDescriptorsPerHeap;
        HeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        HeapDesc.NodeMask = 1;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> HeapPtr;
        HRESULT hr;
        V(Graphics::g_device->CreateDescriptorHeap(
            &HeapDesc, IID_PPV_ARGS(&HeapPtr)));
        sm_DescHeapPool[idx].emplace_back(HeapPtr);
        return HeapPtr.Get();
    }
}

void
DynamicDescriptorHeap::_DiscardDescriptorHeaps(
    D3D12_DESCRIPTOR_HEAP_TYPE HeapType, uint64_t FenceValueForReset,
    const std::vector<ID3D12DescriptorHeap*>& UsedHeaps)
{
    uint32_t idx = HeapType == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ? 1 : 0;
    CriticalSectionScope LockGard(&sm_CS);
    for (auto iter = UsedHeaps.begin(); iter != UsedHeaps.end(); ++iter) {
        sm_RetiredDescHeaps[idx].push(
            std::make_pair(FenceValueForReset, *iter));
    }
}

void
DynamicDescriptorHeap::_RetireCurrentHeap()
{
    // if current heap is unused, do nothing
    if (m_CurrentOffset == 0) {
        ASSERT(m_CurrentHeapPtr == nullptr);
        return;
    }

    ASSERT(m_CurrentHeapPtr != nullptr);
    m_RetiredHeaps.push_back(m_CurrentHeapPtr);
    m_CurrentHeapPtr = nullptr;
    m_CurrentOffset = 0;
}

void
DynamicDescriptorHeap::_RetireUsedHeaps(uint64_t FenceValue)
{
    _DiscardDescriptorHeaps(m_DescType, FenceValue, m_RetiredHeaps);
    m_RetiredHeaps.clear();
}

inline ID3D12DescriptorHeap*
DynamicDescriptorHeap::_GetHeapPointer()
{
    if (m_CurrentHeapPtr == nullptr) {
        ASSERT(m_CurrentOffset == 0);
        m_CurrentHeapPtr = _RequestDescriptorHeap(m_DescType);
        m_FirstDesc = DescriptorHandle(
            m_CurrentHeapPtr->GetCPUDescriptorHandleForHeapStart(),
            m_CurrentHeapPtr->GetGPUDescriptorHandleForHeapStart());
    }
    return m_CurrentHeapPtr;
}

DescriptorHandle
DynamicDescriptorHeap::_Allocate(UINT Count)
{
    DescriptorHandle ret =
        m_FirstDesc + m_CurrentOffset * m_DescSize;
    m_CurrentOffset += Count;
    return ret;
}

void
DynamicDescriptorHeap::_CopyAndBindStagedTables(
    DescriptorHandleCache& HandleCache, ID3D12GraphicsCommandList* CmdList,
    void(STDMETHODCALLTYPE ID3D12GraphicsCommandList::*SetFunc)(
        UINT, D3D12_GPU_DESCRIPTOR_HANDLE))
{
    uint32_t NeededSize = HandleCache._ComputeStagedSize();
    if (!_HasSpace(NeededSize)) {
        _RetireCurrentHeap();
        _UnbindAllValid();
        NeededSize = HandleCache._ComputeStagedSize();
    }

    // This can trigger the creation of a new heap
    m_OwningCtx.SetDescriptorHeap(m_DescType, _GetHeapPointer());
    HandleCache._CopyAndBindStaleTables(m_DescType, m_DescSize,
        _Allocate(NeededSize), CmdList, SetFunc);
}

void
DynamicDescriptorHeap::_UnbindAllValid()
{
    m_GfxHandleCache._UnbindAllValid();
    m_CptHandleCache._UnbindAllValid();
}