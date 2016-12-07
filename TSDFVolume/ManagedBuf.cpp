#include "pch.h"
#include "ManagedBuf.h"

namespace {
    enum ResourceState {
        kNormal = 0,
        kNewBufferCooking,
        kNewBufferReady,
        kRetiringOldBuffer,
        kOldBufferRetired,
        kNumStates
    };

    std::atomic<ResourceState> _bufState(kNormal);
    uint8_t _activeIndex = 0;
    uint64_t _fenceValue = 0;
    std::unique_ptr<thread_guard> _backgroundThread;
};

ManagedBuf::ManagedBuf(DirectX::XMUINT3 reso,
    Type defaultType, Bit defaultBit)
    :_reso(reso),
    _currentType(defaultType),
    _newType(defaultType),
    _deprecatedType(defaultType),
    _currentBit(defaultBit),
    _newBit(defaultBit)
{
}

ManagedBuf::~ManagedBuf()
{
}

void
ManagedBuf::CreateResource()
{
    _CreateVolume(_reso, _currentType, _currentBit, _activeIndex);
}

void
ManagedBuf::Destroy()
{
    ResourceState curState;
    while ((curState = _bufState.load(std::memory_order_acquire)) != kNormal) {
        if (curState == kNewBufferReady) {
            _bufState.store(kOldBufferRetired, std::memory_order_release);
        }
        std::this_thread::yield();
    }
    for (uint i = 0; i < 2; ++i) {
        for (uint j = 0; j < 2; ++j) {
            _typedBuffer[i][j].Destroy();
            _volumeBuffer[i][j].Destroy();
        }
    }
}

bool
ManagedBuf::ChangeResource(const DirectX::XMUINT3& reso,
    const Type bufType, const Bit bufBit)
{
    if ((reso.x == _reso.x && reso.y == _reso.y && reso.z == _reso.z &&
        bufType == _currentType && bufBit == _currentBit) ||
        _bufState.load(std::memory_order_acquire) != kNormal) {
        return false;
    }
    _bufState.store(kNewBufferCooking, std::memory_order_release);
    _newReso = reso;
    _newType = bufType;
    _newBit = bufBit;
    _backgroundThread = std::unique_ptr<thread_guard>(
        new thread_guard(std::thread(&ManagedBuf::_CookBuffer, this,
            reso, bufType, bufBit)));
    return true;
}

ManagedBuf::BufInterface
ManagedBuf::GetResource()
{
    switch (_bufState.load(std::memory_order_acquire)) {
    case kNewBufferReady:
        _activeIndex = 1 - _activeIndex;
        _currentType = _newType;
        _currentBit = _newBit;
        _reso = _newReso;
        _fenceValue = Graphics::g_stats.lastFrameEndFence;
        _bufState.store(kRetiringOldBuffer, std::memory_order_release);
        break;
    case kRetiringOldBuffer:
        if (Graphics::g_cmdListMngr.IsFenceComplete(_fenceValue)) {
            _bufState.store(kOldBufferRetired, std::memory_order_release);
        }
        break;
    }
    BufInterface result;
    result.type = _currentType;
    switch (result.type) {
    case kTypedBuffer:
        result.resource[0] = &_typedBuffer[_activeIndex][0];
        result.resource[1] = &_typedBuffer[_activeIndex][1];
        result.SRV[0] = _typedBuffer[_activeIndex][0].GetSRV();
        result.SRV[1] = _typedBuffer[_activeIndex][1].GetSRV();
        result.UAV[0] = _typedBuffer[_activeIndex][0].GetUAV();
        result.UAV[1] = _typedBuffer[_activeIndex][1].GetUAV();
        break;
    case k3DTexBuffer:
        result.resource[0] = &_volumeBuffer[_activeIndex][0];
        result.resource[1] = &_volumeBuffer[_activeIndex][1];
        result.SRV[0] = _volumeBuffer[_activeIndex][0].GetSRV();
        result.SRV[1] = _volumeBuffer[_activeIndex][1].GetSRV();
        result.UAV[0] = _volumeBuffer[_activeIndex][0].GetUAV();
        result.UAV[1] = _volumeBuffer[_activeIndex][1].GetUAV();
        break;
    }
    return result;
}

void
ManagedBuf::_CreateVolume(const DirectX::XMUINT3 reso,
    const Type bufType, const Bit bufBit, uint targetIdx)
{
    uint32_t volumeBufferElementCount = reso.x * reso.y * reso.z;
    uint32_t elementSize[2];
    DXGI_FORMAT format[2];
    switch (bufBit) {
    case k8Bit:
        elementSize[0] = sizeof(uint8_t);
        format[0] = DXGI_FORMAT_R8_SNORM;
        break;
    case k16Bit:
        elementSize[0] = sizeof(uint16_t);
        format[0] = DXGI_FORMAT_R16_SNORM;
        break;
    case k32Bit:
        elementSize[0] = sizeof(uint32_t);
        format[0] = DXGI_FORMAT_R32_FLOAT;
        break;
    }
    elementSize[1] = sizeof(uint8_t);
    format[1] = DXGI_FORMAT_R8_UINT;
    switch (bufType) {
    case kTypedBuffer:
        _typedBuffer[targetIdx][0].SetFormat(format[0]);
        _typedBuffer[targetIdx][0].Create(L"Typed TSDFVolume Buffer",
            volumeBufferElementCount, elementSize[0]);
        _typedBuffer[targetIdx][1].SetFormat(format[1]);
        _typedBuffer[targetIdx][1].Create(L"Typed WeightVolume Buffer",
            volumeBufferElementCount, elementSize[1]);
        break;
    case k3DTexBuffer:
        _volumeBuffer[targetIdx][0].Create(L"Texture3D TSDFVolume Buffer",
            reso.x, reso.y, reso.z, 1, format[0]);
        _volumeBuffer[targetIdx][1].Create(L"Texture3D WeightVolume Buffer",
            reso.x, reso.y, reso.z, 1, format[1]);
        break;
    }
}

void
ManagedBuf::_CookBuffer(const DirectX::XMUINT3 reso,
    const Type bufType, const Bit bufBit)
{
    _deprecatedType = _currentType;
    _CreateVolume(reso, bufType, bufBit, 1 - _activeIndex);
    _bufState.store(kNewBufferReady, std::memory_order_release);
    while (_bufState.load(std::memory_order_acquire) != kOldBufferRetired) {
        std::this_thread::yield();
    }
    switch (_deprecatedType) {
    case kTypedBuffer:
        _typedBuffer[1 - _activeIndex][0].Destroy();
        _typedBuffer[1 - _activeIndex][1].Destroy();
        break;
    case k3DTexBuffer:
        _volumeBuffer[1 - _activeIndex][0].Destroy();
        _volumeBuffer[1 - _activeIndex][1].Destroy();
        break;
    }
    _bufState.store(kNormal, std::memory_order_release);
}