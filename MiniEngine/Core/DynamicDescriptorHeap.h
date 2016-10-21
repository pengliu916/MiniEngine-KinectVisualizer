#pragma once
#include <vector>
#include <queue>
#include "DescriptorHeap.h"
#include "RootSignature.h"

class DynamicDescriptorHeap
{
public:
    DynamicDescriptorHeap(CommandContext& OwningContext);
    ~DynamicDescriptorHeap();

    static void Initialize();
    static void Shutdown();
    static void DestroyAll();
    static uint32_t GetDescriptorSize();

    void CleanupUsedHeaps(uint64_t fenceValue);
    D3D12_GPU_DESCRIPTOR_HANDLE UploadDirect(
        D3D12_CPU_DESCRIPTOR_HANDLE Handles);

    void SetGraphicsDescriptorHandles(UINT RootIndex, UINT Offset,
        UINT NumHandles, const D3D12_CPU_DESCRIPTOR_HANDLE Handles[]) {
        m_GraphicsHandleCache._StageDescriptorHandles(
            RootIndex, Offset, NumHandles, Handles);
    }

    void SetComputeDescriptorHandles(UINT RootIndex, UINT Offset,
        UINT NumHandles, const D3D12_CPU_DESCRIPTOR_HANDLE Handles[]) {
        m_ComputeHandleCache._StageDescriptorHandles(
            RootIndex, Offset, NumHandles, Handles);
    }

    void ParseGraphicsRootSignature(const RootSignature& RootSig) {
        m_GraphicsHandleCache._ParseRootSignature(RootSig);
    }

    void ParseComputeRootSignature(const RootSignature& RootSig) {
        m_ComputeHandleCache._ParseRootSignature(RootSig);
    }

    inline void CommitGraphicsRootDescriptorTables(
        ID3D12GraphicsCommandList* CmdList) {
        if (m_GraphicsHandleCache.m_StaleRootParamsBitMap != 0) {
            _CopyAndBindStagedTables(m_GraphicsHandleCache, CmdList,
                &ID3D12GraphicsCommandList::SetGraphicsRootDescriptorTable);
        }
    }

    inline void CommitComputeRootDescriptorTables(
        ID3D12GraphicsCommandList* CmdList) {
        if (m_ComputeHandleCache.m_StaleRootParamsBitMap != 0) {
            _CopyAndBindStagedTables(m_ComputeHandleCache, CmdList,
                &ID3D12GraphicsCommandList::SetComputeRootDescriptorTable);
        }
    }

private:
    struct DescriptorTableCache {
        DescriptorTableCache() : AssignedHandlesBitMap(0) {}
        uint32_t AssignedHandlesBitMap;
        D3D12_CPU_DESCRIPTOR_HANDLE* TableStart;
        uint32_t TableSize;
    };

    struct DescriptorHandleCache
    {
        DescriptorHandleCache() {
            _ClearCache();
        }

        void _ClearCache() {
            m_RootDescriptorTablesBitMap = 0;
            m_MaxCachedDescriptors = 0;
        }

        uint32_t _ComputeStagedSize();

        void _CopyAndBindStaleTables(DescriptorHandle DestHandleStart,
            ID3D12GraphicsCommandList* CmdList,
            void(STDMETHODCALLTYPE ID3D12GraphicsCommandList::*SetFunc)(
                UINT, D3D12_GPU_DESCRIPTOR_HANDLE));

        void _UnbindAllValid();
        void _StageDescriptorHandles(UINT RootIndex, UINT Offset,
            UINT NumHandles, const D3D12_CPU_DESCRIPTOR_HANDLE Handles[]);
        void _ParseRootSignature(const RootSignature& RootSig);

        static const uint32_t kMaxNumDescriptors = 256;
        static const uint32_t kMaxNumDescriptorTables = 16;

        uint32_t m_RootDescriptorTablesBitMap;
        uint32_t m_StaleRootParamsBitMap;
        uint32_t m_MaxCachedDescriptors;

        DescriptorTableCache m_RootDescriptorTable[kMaxNumDescriptorTables];
        D3D12_CPU_DESCRIPTOR_HANDLE m_HandleCache[kMaxNumDescriptors];
    };

    static ID3D12DescriptorHeap* _RequestDescriptorHeap();
    static void _DiscardDescriptorHeaps(uint64_t FenceValueForReset,
        const std::vector<ID3D12DescriptorHeap*>& UsedHeaps);

    bool _HasSpace(uint32_t Count) {
        return (m_CurrentHeapPtr != nullptr &&
            m_CurrentOffset + Count <= kNumDescriptorsPerHeap);
    }

    void _RetireCurrentHeap();
    void _RetireUsedHeaps(uint64_t FenceValue);
    ID3D12DescriptorHeap* _GetHeapPointer();
    DescriptorHandle _Allocate(UINT Count);
    void _CopyAndBindStagedTables(DescriptorHandleCache& HandleCache,
        ID3D12GraphicsCommandList* CmdList,
        void(STDMETHODCALLTYPE ID3D12GraphicsCommandList::*SetFunc)(
            UINT, D3D12_GPU_DESCRIPTOR_HANDLE));
    void _UnbindAllValid();

    static const uint32_t kNumDescriptorsPerHeap = 1024;
    static CRITICAL_SECTION sm_CS;
    static std::vector<Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>>
        sm_DescriptorHeapPool;
    static std::queue<std::pair<uint64_t, ID3D12DescriptorHeap*>>
        sm_RetiredDescriptorHeaps;
    static std::queue<ID3D12DescriptorHeap*> sm_AvailableDescriptorHeaps;
    static uint32_t sm_DescriptorSize;

    DescriptorHandleCache m_GraphicsHandleCache;
    DescriptorHandleCache m_ComputeHandleCache;
    CommandContext& m_OwningContext;
    ID3D12DescriptorHeap* m_CurrentHeapPtr;
    uint32_t m_CurrentOffset;
    DescriptorHandle m_FirstDescriptor;
    std::vector<ID3D12DescriptorHeap*> m_RetiredHeaps;
};