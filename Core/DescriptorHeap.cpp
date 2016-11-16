#include "LibraryHeader.h"
#include "Graphics.h"
#include "Utility.h"
#include "DescriptorHeap.h"

DescriptorHeap::DescriptorHeap(
    UINT maxDescriptors, D3D12_DESCRIPTOR_HEAP_TYPE type, bool shaderVisible)
    : mMaxSize(maxDescriptors)
{
    HRESULT hr;

    D3D12_DESCRIPTOR_HEAP_DESC desc = {};
    desc.Type = type;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    desc.NumDescriptors = mMaxSize;

    V(Graphics::g_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&mHeap)));

    mShaderVisible = shaderVisible;

    mCPUBegin = mHeap->GetCPUDescriptorHandleForHeapStart();
    mHandleIncrementSize =
        Graphics::g_device->GetDescriptorHandleIncrementSize(desc.Type);

    if (shaderVisible) {
        mGPUBegin = mHeap->GetGPUDescriptorHandleForHeapStart();
    }
}

DescriptorHandle DescriptorHeap::Append()
{
    ASSERT(mCurrentSize < mMaxSize);
    DescriptorHandle ret(CPU(mCurrentSize), GPU(mCurrentSize));
    mCurrentSize++;
    return ret;
}